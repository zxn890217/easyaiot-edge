/*
 * mpp_glibc_patch.c -- rewire one versioned dependency inside an ELF64 shared
 * object, in place, without changing its size or layout.
 *
 * WHY THIS EXISTS
 * ---------------
 * The board ships a prebuilt librockchip_mpp.so.0 (host is Ubuntu 20.04,
 * glibc 2.31). Its .gnu.version_r asks for four glibc symbol versions:
 *
 *     GLIBC_2.17 / GLIBC_2.27 / GLIBC_2.28  -> satisfied by the container
 *     GLIBC_2.29                            -> NOT satisfied by the container
 *
 * The video-service container is AlmaLinux 8.10 (platform:el8) with glibc
 * 2.28. Exactly three symbols hang off the 2.29 node and they are all libm:
 *
 *     undefined reference to `pow@GLIBC_2.29'
 *     undefined reference to `log@GLIBC_2.29'
 *     undefined reference to `log2@GLIBC_2.29'
 *
 * Approaches that were tried and are structurally dead, in the order they were
 * ruled out (each was measured, not assumed):
 *
 *   - Link against the container's libm. The library already carries
 *     DT_NEEDED libm.so.6, and /lib64/libm.so.6 publishes no GLIBC_2.29
 *     version node at all (readelf -V). Passing -lm ahead of the object, to
 *     defeat --as-needed, reproduces all three errors verbatim.
 *   - A shim .so defining pow/log/log2 at GLIBC_2.29. This DOES satisfy the
 *     static linker once a --version-script declares the node, so COMPILE_OK
 *     is reachable. It then dies at execution:
 *         ./mpp_probe_in: /lib64/libm.so.6: version `GLIBC_2.29' not found
 *         (required by /tmp/librockchip_mpp.so.1)
 *     ld.so's startup version audit does not search symbols. It lifts the
 *     vn_file string out of DT_VERNEED ("libm.so.6"), resolves that one name
 *     to a loaded object (find_needed) and looks for the version node only
 *     inside it. A preloaded shim under any other name can never be consulted;
 *     under the name libm.so.6 it shadows the real libm and sin/cos/sqrt/exp
 *     all go missing. So LD_PRELOAD is dead too -- and not for the reason
 *     usually given for avoiding it.
 *   - Building MPP from source in the container: no source on the board, and
 *     no egress (curl to github times out).
 *   - ffmpeg's h264_v4l2m2m: the encoder is present, but /dev/video-enc0 on
 *     the host is a 4-byte regular file rather than a character device, and
 *     the container has no /dev/video* at all.
 *
 * WHAT THIS TOOL DOES
 * -------------------
 * Points the GLIBC_2.29 requirement of those three symbols at a version the
 * container does provide. glibc keeps the pre-2.29 implementations exported
 * under GLIBC_2.17 (on aarch64, 2.17 is the baseline for every base symbol),
 * so pow/log/log2 resolve to the real thing.
 *
 * Both names are 10 characters, so the replacement lands in the string's own
 * slot -- nothing moves, which is what keeps this a two-field edit instead of
 * an ELF editor. vna_hash has to be recomputed in the same breath: ld.so
 * compares the hash before the name, so a patched string with a stale hash
 * fails exactly like an unpatched one.
 *
 * HONEST COST
 * -----------
 * You end up shipping a modified vendor binary. Deterministic and auditable,
 * but not the same as a clean build. The redirected functions are used by the
 * rate-control maths and the 2.17 aarch64 pow predates the correctly-rounded
 * rewrite (a couple of ulp at worst), so the realistic failure mode here is
 * bitrate drift, not a crash. Before any of this goes near production, encode
 * one synthetic NV12 frame with the patched library inside the container and
 * diff that bitstream against the same probe running natively on the host.
 * Byte equality is the bar.
 *
 * The ELF structs below are declared inline rather than pulled from <elf.h>.
 * That is deliberate: the header is absent from some cross toolchains and this
 * tool has to build in a container whose package set we do not control.
 *
 * USAGE
 * -----
 *   gcc -O2 mpp_glibc_patch.c -o mpp_glibc_patch
 *   ./mpp_glibc_patch in.so                              # list Verneed entries
 *   ./mpp_glibc_patch in.so out.so GLIBC_2.29 GLIBC_2.17 [vn_file]
 *
 * The input is never modified; output goes to a new file. Exit status: 0 ok,
 * 1 usage or no match, 2 the file was not what this tool can handle.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal ELF64 surface, matching the psABI layout exactly ---- */

#define MPP_MAGIC       "\177ELF"
#define MPP_CLASS64     2
#define MPP_DATA2LSB    1
#define MPP_EM_AARCH64  183
#define MPP_SHT_STRTAB  3
#define MPP_SHT_VERNEED 0x6ffffffe

typedef uint64_t MppAddr;
typedef uint64_t MppXword;
typedef uint32_t MppWord;
typedef uint16_t MppHalf;

typedef struct
{
  unsigned char e_ident[16];
  MppHalf e_type;
  MppHalf e_machine;
  MppWord e_version;
  MppAddr e_entry;
  MppAddr e_phoff;
  MppAddr e_shoff;
  MppWord e_flags;
  MppHalf e_ehsize;
  MppHalf e_phentsize;
  MppHalf e_phnum;
  MppHalf e_shentsize;
  MppHalf e_shnum;
  MppHalf e_shstrndx;
} MppEhdr;

typedef struct
{
  MppWord sh_name;
  MppWord sh_type;
  MppXword sh_flags;
  MppAddr sh_addr;
  MppAddr sh_offset;
  MppXword sh_size;
  MppWord sh_link;
  MppWord sh_info;
  MppXword sh_addralign;
  MppXword sh_entsize;
} MppShdr;

typedef struct
{
  MppHalf vn_version;
  MppHalf vn_cnt;
  MppWord vn_file;
  MppWord vn_aux;
  MppWord vn_next;
} MppVerneed;

typedef struct
{
  MppWord vna_hash;
  MppHalf vna_flags;
  MppHalf vna_other;
  MppWord vna_name;
  MppWord vna_next;
} MppVernaux;

/* The classic ELF hash -- the one glibc's _dl_elf_hash uses for version names
 * and the one GNU ld stores in vna_hash. It is NOT the GNU hash that backs
 * .gnu.symbol lookups. 32-bit arithmetic is safe: bits above 31 that the
 * reference 64-bit accumulator can grow never feed back into the low 32 and
 * are dropped by the uint32_t return. */
static uint32_t
elf_hash (const char *name)
{
  uint32_t h = 0;
  while (*name)
    {
      uint32_t g;
      h = (h << 4) + (unsigned char) *name++;
      g = h & 0xf0000000u;
      if (g)
        h ^= g >> 24;
      h &= ~g;
    }
  return h;
}

static int
read_file (const char *path, unsigned char **out, size_t *out_len)
{
  FILE *f = fopen (path, "rb");
  if (!f)
    return -1;
  if (fseek (f, 0, SEEK_END) != 0)
    {
      fclose (f);
      return -1;
    }
  long len = ftell (f);
  if (len < (long) sizeof (MppEhdr))
    {
      fclose (f);
      return -1;
    }
  rewind (f);
  unsigned char *buf = malloc ((size_t) len);
  if (!buf)
    {
      fclose (f);
      return -1;
    }
  if (fread (buf, 1, (size_t) len, f) != (size_t) len)
    {
      free (buf);
      fclose (f);
      return -1;
    }
  fclose (f);
  *out = buf;
  *out_len = (size_t) len;
  return 0;
}

static int
write_file (const char *path, const unsigned char *buf, size_t len)
{
  FILE *f = fopen (path, "wb");
  if (!f)
    return -1;
  int ok = (fwrite (buf, 1, len, f) == len) && (fclose (f) == 0);
  return ok ? 0 : -1;
}

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      fprintf (stderr,
               "usage: %s IN.so [OUT.so FROM_VERSION TO_VERSION [VN_FILE]]\n",
               argv[0]);
      return 1;
    }

  const int listing = (argc < 5);
  const char *from = listing ? NULL : argv[3];
  const char *to = listing ? NULL : argv[4];
  const char *file_filter = (argc >= 6) ? argv[5] : NULL;

  if (!listing && strlen (to) > strlen (from))
    {
      /* Names are rewritten where they lie in .dynstr. A longer TO_VERSION
       * would walk into the following string, so it is refused outright. */
      fprintf (stderr,
               "refusing: \"%s\" is longer than \"%s\"; the replacement has to "
               "fit the existing .dynstr slot\n", to, from);
      return 1;
    }

  size_t len = 0;
  unsigned char *buf = NULL;
  /* Declared before the first goto so every done: path sees a defined pointer;
   * snap in particular is NULL for all the early exits. */
  char *snap = NULL;
  if (read_file (argv[1], &buf, &len) != 0)
    {
      fprintf (stderr, "cannot read %s: %s\n", argv[1], strerror (errno));
      return 2;
    }

  const MppEhdr *eh = (const MppEhdr *) buf;
  int rc = 0;

  if (memcmp (eh->e_ident, MPP_MAGIC, 4) != 0)
    {
      fprintf (stderr, "%s is not an ELF object\n", argv[1]);
      rc = 2;
      goto done;
    }
  if (eh->e_ident[4] != MPP_CLASS64 || eh->e_ident[5] != MPP_DATA2LSB)
    {
      fprintf (stderr, "need a little-endian ELF64 object\n");
      rc = 2;
      goto done;
    }
  if (eh->e_machine != MPP_EM_AARCH64)
    fprintf (stderr, "note: e_machine is %u, not EM_AARCH64 (%u)\n",
             (unsigned) eh->e_machine, MPP_EM_AARCH64);

  if (eh->e_shentsize < sizeof (MppShdr)
      || eh->e_shoff > len
      || (size_t) eh->e_shnum * eh->e_shentsize > len - eh->e_shoff)
    {
      fprintf (stderr, "section headers run past the end of the file\n");
      rc = 2;
      goto done;
    }
  if (eh->e_shstrndx >= eh->e_shnum)
    {
      fprintf (stderr, "bad e_shstrndx\n");
      rc = 2;
      goto done;
    }

  const MppShdr *shdrs = (const MppShdr *) (buf + eh->e_shoff);
  const MppShdr *shstr_sec = &shdrs[eh->e_shstrndx];
  if (shstr_sec->sh_offset > len
      || shstr_sec->sh_size > len - shstr_sec->sh_offset)
    {
      fprintf (stderr, ".shstrtab points outside the file\n");
      rc = 2;
      goto done;
    }
  const char *shstr = (const char *) (buf + shstr_sec->sh_offset);

  /* Two sections suffice: the Verneed chain holds the hashes, .dynstr holds
   * the names the chain points into. */
  const MppShdr *verneed = NULL;
  const MppShdr *dynstr = NULL;
  for (int i = 0; i < eh->e_shnum; ++i)
    {
      const MppShdr *s = &shdrs[i];
      if (s->sh_type == MPP_SHT_VERNEED && !verneed)
        verneed = s;
      if (s->sh_type == MPP_SHT_STRTAB && s->sh_name < shstr_sec->sh_size
          && strcmp (shstr + s->sh_name, ".dynstr") == 0)
        dynstr = s;
    }

  if (!verneed)
    {
      fprintf (stderr, "no .gnu.version_r section: nothing to patch\n");
      rc = 1;
      goto done;
    }
  if (!dynstr)
    {
      fprintf (stderr, "no .dynstr section: cannot resolve version names\n");
      rc = 2;
      goto done;
    }
  if (verneed->sh_offset > len || verneed->sh_size > len - verneed->sh_offset)
    {
      fprintf (stderr, ".gnu.version_r points outside the file\n");
      rc = 2;
      goto done;
    }
  if (dynstr->sh_offset > len || dynstr->sh_size > len - dynstr->sh_offset)
    {
      fprintf (stderr, ".dynstr points outside the file\n");
      rc = 2;
      goto done;
    }

  const char *strtab = (const char *) (buf + dynstr->sh_offset);
  const size_t strsz = (size_t) dynstr->sh_size;
  unsigned char *vbase = buf + verneed->sh_offset;
  const size_t vsz = (size_t) verneed->sh_size;

  /* Reads of version names go through a snapshot, writes go into buf.
   *
   * GNU ld pools identical strings in .dynstr, so two Vernaux entries -- one
   * for libm.so.6, one for libc.so.6 -- can share the offset for "GLIBC_2.29".
   * Comparing straight out of buf would then rewrite the string on the first
   * hit and make the second one no longer compare equal, leaving its vna_hash
   * pointing at a version nobody provides while its name reads GLIBC_2.17. The
   * snapshot keeps every comparison on the original bytes, so all sharers of
   * that offset get their hash fixed. */
  snap = malloc (strsz + 1);
  if (!snap)
    {
      fprintf (stderr, "out of memory\n");
      rc = 2;
      goto done;
    }
  memcpy (snap, strtab, strsz);
  snap[strsz] = '\0';

  /* Bounds helpers. Offsets inside these sections come from a vendor binary we
   * are about to rewrite, so every one is checked before it is dereferenced. */
#define IN_SECTION(sec_off, sec_sz, off, need)                        \
  ((size_t) (off) >= (sec_off) && (size_t) (off) < (sec_off) + (sec_sz) \
   && (size_t) (off) - (sec_off) + (need) <= (sec_sz))
#define STR_OK(off) ((size_t) (off) < strsz)

  int hits = 0;
  int bad_hash = 0;
  MppVerneed *vn = (MppVerneed *) vbase;
  for (MppWord n = 0; n < verneed->sh_info && vn; ++n)
    {
      if (!IN_SECTION (verneed->sh_offset, vsz, (unsigned char *) vn - buf,
                       sizeof (MppVerneed)))
        {
          fprintf (stderr, "Verneed entry %u falls outside .gnu.version_r\n", n);
          rc = 2;
          goto done;
        }
      const char *vfile = NULL;
      if (STR_OK (vn->vn_file))
        vfile = snap + vn->vn_file;
      MppVernaux *aux = (MppVernaux *) ((unsigned char *) vn + vn->vn_aux);
      for (MppWord a = 0; a < vn->vn_cnt && aux; ++a)
        {
          if (!IN_SECTION (verneed->sh_offset, vsz, (unsigned char *) aux - buf,
                           sizeof (MppVernaux)))
            {
              fprintf (stderr, "Vernaux %u of entry %u is out of bounds\n", a, n);
              rc = 2;
              goto done;
            }
          const char *vname = STR_OK (aux->vna_name) ? snap + aux->vna_name
                                                     : NULL;
          if (!vname || !vfile)
            {
              fprintf (stderr, "version name offset is out of .dynstr\n");
              rc = 2;
              goto done;
            }

          if (listing)
            {
              /* List mode doubles as a self test. Every hash in this table was
               * written by GNU ld, so if elf_hash above reproduces the ones for
               * GLIBC_2.17 / 2.27 / 2.28 in this very file, it is the same
               * function ld used and the value written by a patch run is right. */
              const uint32_t want = elf_hash (vname);
              const int agree = (want == aux->vna_hash);
              printf ("verneed file=%-16s version=%-14s hash=0x%08x "
                      "expected=0x%08x %s idx=%u\n", vfile, vname,
                      aux->vna_hash, want, agree ? "OK" : "MISMATCH",
                      (unsigned) aux->vna_other);
              if (!agree)
                bad_hash = 1;
            }
          else if (strcmp (vname, from) == 0
                   && (!file_filter || strcmp (vfile, file_filter) == 0))
            {
              const uint32_t want = elf_hash (vname);
              if (want != aux->vna_hash)
                {
                  /* Refuse rather than write a hash produced by a function that
                   * just failed to describe this file. */
                  fprintf (stderr,
                           "elf_hash(\"%s\") gave 0x%08x but the file says "
                           "0x%08x; refusing to write\n", vname, want,
                           aux->vna_hash);
                  rc = 2;
                  goto done;
                }
              /* The tail is zero padded so a shorter TO_VERSION cannot leave a
               * stray fragment behind that some other offset alias into. */
              const size_t room = strlen (from) + 1;
              if ((size_t) aux->vna_name + room > strsz)
                {
                  fprintf (stderr, "no room for the replacement in .dynstr\n");
                  rc = 2;
                  goto done;
                }
              char *slot = (char *) buf + dynstr->sh_offset + aux->vna_name;
              const uint32_t old_hash = aux->vna_hash;
              memcpy (slot, to, strlen (to));
              memset (slot + strlen (to), 0, room - strlen (to));
              aux->vna_hash = elf_hash (to);
              printf ("patched: file=%s %s -> %s  hash 0x%08x -> 0x%08x  "
                      "(dynstr+0x%x)\n", vfile, from, to, old_hash,
                      aux->vna_hash, (unsigned) aux->vna_name);
              ++hits;
            }

          if (!aux->vna_next)
            break;
          aux = (MppVernaux *) ((unsigned char *) aux + aux->vna_next);
        }
      if (!vn->vn_next)
        break;
      vn = (MppVerneed *) ((unsigned char *) vn + vn->vn_next);
    }
#undef IN_SECTION
#undef STR_OK

  if (listing)
    {
      if (bad_hash)
        {
          printf ("\nSELFTEST FAILED: at least one stored hash does not match "
                  "elf_hash() of its name, so elf_hash() is the wrong function "
                  "and a patch run here would corrupt the file.\n");
          rc = 2;
        }
      else
        {
          printf ("\nselftest OK: every stored hash reproduced, elf_hash() "
                  "matches this file's linker.\n");
          rc = 0;
        }
    }
  else if (hits == 0)
    {
      fprintf (stderr, "no Verneed entry named %s%s%s was found -- nothing "
                       "written\n", from,
               file_filter ? " for file " : "", file_filter ? file_filter : "");
      rc = 1;
    }
  else if (write_file (argv[2], buf, len) != 0)
    {
      fprintf (stderr, "cannot write %s: %s\n", argv[2], strerror (errno));
      rc = 2;
    }
  else
    {
      printf ("%d entr%s patched; %s written, %s left untouched\n", hits,
              hits == 1 ? "y" : "ies", argv[2], argv[1]);
      printf ("next: make sure the OUTPUT is the file the SONAME resolves to, "
              "its SONAME is librockchip_mpp.so.1\n");
    }

done:
  free (snap);
  free (buf);
  return rc;
}
