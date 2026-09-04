import os
import sys
import ctypes

os.environ["TRON"] = "1"
os.environ["TRTAG"] = sys.argv[3]
m = ctypes.CDLL(sys.argv[1])
print("rknn_init =", m.rknn_init(ctypes.c_char_p(sys.argv[2].encode()), None, 0, 0))
sys.stdout.flush()
os._exit(0)
