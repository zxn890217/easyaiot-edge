"""模型导出服务：把训练权重异步转换为边缘端可加载的格式（RK3588 .rknn / .onnx）。

转换在控制面（x86_64 Linux + rknn-toolkit2）完成，产物写入 flat/MinIO 对象存储的
`exports` bucket（key: `model_{id}/{format}/model.{ext}`），并把路径回填到
`model` 表对应列，供 OTA 选路与 RUNTIME 拉取使用。

rknn-toolkit2 单次 build 往往数分钟，因此任务落库后由后台线程执行，
前端通过 /model/export/status/<id> 轮询（PENDING → PROCESSING → COMPLETED|FAILED）。
"""
from __future__ import annotations

import logging
import os
import shutil
import subprocess
import tempfile
import threading
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from app.services.minio_service import ModelService, parse_minio_download_url

logger = logging.getLogger(__name__)

EXPORT_BUCKET = 'exports'
RKNN_SCRIPT_NAME = 'ensure_rknn_model.py'
ONNX_SCRIPT_NAME = 'ensure_onnx_model.py'

#: 已实现真实转换逻辑的格式；其余格式前端虽可选，但直接拒绝并说明原因
SUPPORTED_FORMATS = ('rknn', 'onnx')
EXT_BY_FORMAT = {'rknn': 'rknn', 'onnx': 'onnx'}

STATUS_PENDING = 'PENDING'
STATUS_PROCESSING = 'PROCESSING'
STATUS_COMPLETED = 'COMPLETED'
STATUS_FAILED = 'FAILED'


class ExportError(Exception):
    """导出流程中的可预期错误（参数不合法、源文件缺失、转换失败等）。"""


def _repo_root() -> Path:
    video_root = Path(__file__).resolve().parents[2]
    sibling_runtime = video_root.parent / 'RUNTIME'
    if sibling_runtime.is_dir():
        return video_root.parent
    opt = Path('/opt/easyaiot')
    if (opt / 'RUNTIME').is_dir():
        return opt
    return video_root.parent


def _conversion_script(export_format: str) -> Path:
    name = RKNN_SCRIPT_NAME if export_format == 'rknn' else ONNX_SCRIPT_NAME
    return _repo_root() / 'RUNTIME' / 'scripts' / name


def _python_for_export() -> str:
    """Prefer the interpreter that carries the toolchain (ultralytics / rknn-toolkit2)."""
    from app.services.runtime_config_service import _python_for_export as inherited

    return inherited()


def _export_timeout(export_format: str) -> int:
    default = '1800' if export_format == 'rknn' else '900'
    key = 'RUNTIME_RKNN_EXPORT_TIMEOUT' if export_format == 'rknn' else 'RUNTIME_ONNX_EXPORT_TIMEOUT'
    try:
        return max(60, int(os.getenv(key, default) or default))
    except ValueError:
        return int(default)


def _resolve_bucket_key(path: str) -> Tuple[Optional[str], Optional[str]]:
    """把 model_path / onnx_model_path 解析成 (bucket, object_key)。"""
    raw = (path or '').strip()
    if not raw:
        return None, None
    bucket, key = parse_minio_download_url(raw)
    if bucket and key:
        return bucket, key
    if '/' in raw and not os.path.isabs(raw) and not raw.startswith('http'):
        parts = raw.split('/', 1)
        return parts[0], parts[1]
    return None, None


def _materialize_source(model, workdir: Path) -> Tuple[Path, str]:
    """Download the best available source weight into workdir.

    Returns (local_path, source_kind) where source_kind is 'onnx' or 'pt'.
    """
    candidates: List[Tuple[str, str]] = []
    if getattr(model, 'onnx_model_path', None):
        candidates.append(('onnx', model.onnx_model_path))
    if getattr(model, 'model_path', None):
        candidates.append(('auto', model.model_path))

    for kind, stored in candidates:
        bucket, object_key = _resolve_bucket_key(stored)
        if not bucket or not object_key:
            if os.path.isfile(stored):
                ext = os.path.splitext(stored)[1].lower()
                return _link_source(stored, workdir, ext), ('onnx' if ext == '.onnx' else 'pt')
            continue
        ext = os.path.splitext(object_key)[1].lower() or ('.onnx' if kind == 'onnx' else '.pt')
        dest = workdir / f'source{ext}'
        ok, err = ModelService.download_from_minio(bucket, object_key, str(dest))
        if not ok or not dest.is_file() or dest.stat().st_size <= 0:
            logger.warning('导出源下载失败 %s/%s: %s', bucket, object_key, err)
            continue
        return dest, ('onnx' if ext == '.onnx' else 'pt')

    raise ExportError('模型没有可用于转换的权重文件（请先上传 .pt 或 .onnx）')


def _link_source(src: str, workdir: Path, ext: str) -> Path:
    dest = workdir / f'source{ext}'
    shutil.copy2(src, dest)
    return dest


def _run_conversion_script(
    script: Path,
    args: List[str],
    *,
    timeout: int,
    env_extra: Optional[Dict[str, str]] = None,
) -> str:
    if not script.is_file():
        raise ExportError(f'转换脚本缺失: {script}')
    cmd = [_python_for_export(), str(script)] + args
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    logger.info('模型导出执行: %s', ' '.join(cmd))
    try:
        completed = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            env=env,
        )
    except subprocess.TimeoutExpired as exc:
        raise ExportError(f'转换超时（>{timeout}s）: {exc}') from exc

    output = ((completed.stdout or '') + '\n' + (completed.stderr or '')).strip()
    if completed.returncode != 0:
        tail = output[-1500:] or f'exit={completed.returncode}'
        raise ExportError(f'{script.name} 失败 rc={completed.returncode}: {tail}')
    logger.info('模型导出脚本输出: %s', output[-2000:])
    return output


def _upload_artifacts(export_format: str, model_id: int, produced: List[Path]) -> Tuple[str, int]:
    """Upload converted files to the exports bucket. Returns (export_path, size_bytes)."""
    ext = EXT_BY_FORMAT[export_format]
    primary = next((p for p in produced if p.suffix.lower() == f'.{ext}'), None)
    if primary is None or not primary.is_file():
        raise ExportError(f'转换未产出 {ext} 文件')

    object_key = f'model_{model_id}/{export_format}/{primary.name}'
    ok, err = ModelService.upload_to_minio(EXPORT_BUCKET, object_key, str(primary))
    if not ok:
        raise ExportError(f'产物上传失败: {err}')

    for extra in produced:
        if extra == primary or not extra.is_file():
            continue
        ModelService.upload_to_minio(EXPORT_BUCKET, f'model_{model_id}/{export_format}/{extra.name}', str(extra))

    return f'{EXPORT_BUCKET}/{object_key}', primary.stat().st_size


def _convert_from_source(
    *,
    script: Path,
    source: Path,
    source_kind: str,
    workdir: Path,
    export_format: str,
    params: Dict[str, Any],
    timeout: int,
) -> Tuple[List[Path], int]:
    """Run the conversion script; returns (artifacts, imgsz_used)."""
    ext = EXT_BY_FORMAT[export_format]
    target = workdir / f'model.{ext}'
    imgsz = int(params.get('img_size') or params.get('imgsz') or 640)
    passthrough = export_format == 'onnx' and source_kind == 'onnx'

    if passthrough:
        shutil.copy2(source, target)
        args: List[str] = []
    elif export_format == 'rknn':
        args = [
            '--input', str(source),
            '--output', str(target),
            '--imgsz', str(imgsz),
            '--target-platform', str(params.get('target_platform') or 'rk3588'),
            '--opt-level', str(params.get('opt_level') or 3),
        ]
        if params.get('force'):
            args.append('--force')
        if params.get('quantization') or params.get('quantized'):
            args.append('--quantized')
        else:
            args.append('--no-quant')
        dataset = str(params.get('dataset') or '').strip()
        if dataset:
            args += ['--dataset', _stage_dataset(dataset, workdir)]
    else:
        args = ['--input', str(source), '--output', str(target), '--imgsz', str(imgsz)]
        if params.get('force'):
            args.append('--force')

    if not passthrough:
        _run_conversion_script(
            script,
            args,
            timeout=timeout,
            env_extra={'RKNN_TARGET_PLATFORM': str(params.get('target_platform') or 'rk3588')},
        )

    artifacts = [target, target.with_suffix('.names'), target.with_suffix('.rknn.json')]
    existing = [p for p in artifacts if p.is_file()]
    return existing, imgsz


def _stage_dataset(dataset: str, workdir: Path) -> str:
    """Resolve a calibration dataset.txt (bucket key, URL or local path) to a local file."""
    if os.path.isfile(dataset):
        dest = workdir / 'dataset.txt'
        shutil.copy2(dataset, dest)
        return str(dest)
    bucket, object_key = _resolve_bucket_key(dataset)
    if bucket and object_key:
        dest = workdir / Path(object_key).name
        ok, err = ModelService.download_from_minio(bucket, object_key, str(dest))
        if ok and dest.is_file():
            return str(dest)
        logger.warning('校准数据集下载失败 %s/%s: %s', bucket, object_key, err)
    raise ExportError(f'校准数据集不可访问: {dataset}')


def _app():
    from flask import current_app

    return current_app._get_current_object()


def create_export_task(model_id: int, export_format: str, params: Dict[str, Any]) -> Dict[str, Any]:
    """Validate + persist an export task, then convert it in a background thread."""
    from models import AiModel, ModelExport, db

    fmt = (export_format or '').strip().lower()
    if fmt not in SUPPORTED_FORMATS:
        supported = '/'.join(SUPPORTED_FORMATS)
        raise ExportError(f'暂不支持 {export_format or "未知"} 格式导出，当前支持: {supported}')

    model = AiModel.query.get(model_id)
    if model is None:
        raise ExportError(f'模型不存在: {model_id}')

    params = dict(params or {})
    row = ModelExport(
        model_id=model.id,
        model_name=model.name,
        model_path=model.onnx_model_path or model.model_path,
        export_format=fmt,
        status=STATUS_PENDING,
        target_platform=str(params.get('target_platform') or 'rk3588') if fmt == 'rknn' else None,
        quantized=bool(params.get('quantization') or params.get('quantized')),
        imgsz=int(params.get('img_size') or params.get('imgsz') or 640),
        dataset=str(params.get('dataset') or '') or None,
    )
    db.session.add(row)
    db.session.commit()

    app = _app()
    payload = dict(params)
    thread = threading.Thread(
        target=_worker,
        args=(app, row.id, model.id, fmt, payload),
        name=f'model-export-{row.id}',
        daemon=True,
    )
    thread.start()
    logger.info('已提交模型导出任务: export_id=%s model_id=%s format=%s', row.id, model.id, fmt)
    return serialize_export(row)


def _worker(app, export_id: int, model_id: int, export_format: str, params: Dict[str, Any]) -> None:
    with app.app_context():
        from models import AiModel, ModelExport, db

        workdir = Path(tempfile.mkdtemp(prefix=f'model_export_{export_id}_'))
        try:
            row = ModelExport.query.get(export_id)
            if row is None:
                return
            row.status = STATUS_PROCESSING
            row.error_message = None
            db.session.commit()

            model = AiModel.query.get(model_id)
            if model is None:
                raise ExportError(f'模型不存在: {model_id}')

            artifacts, imgsz = _convert_source_for(
                model=model,
                workdir=workdir,
                export_format=export_format,
                params=params,
            )
            export_path, size_bytes = _upload_artifacts(export_format, model_id, artifacts)

            setattr(model, f'{export_format}_model_path', export_path)
            row.export_path = export_path
            row.size_bytes = size_bytes
            row.imgsz = imgsz or row.imgsz
            row.status = STATUS_COMPLETED
            row.error_message = None
            db.session.commit()
            logger.info(
                '模型导出完成: export_id=%s model_id=%s format=%s path=%s size=%s',
                export_id, model_id, export_format, export_path, size_bytes,
            )
        except Exception as exc:  # noqa: BLE001 - background task must never crash the host
            logger.error('模型导出失败 export_id=%s: %s', export_id, exc, exc_info=True)
            try:
                db.session.rollback()
            except Exception:
                pass
            try:
                row = ModelExport.query.get(export_id)
                if row is not None:
                    row.status = STATUS_FAILED
                    row.error_message = str(exc)[:2000]
                    db.session.commit()
            except Exception:
                db.session.rollback()
        finally:
            shutil.rmtree(workdir, ignore_errors=True)


def _convert_source_for(*, model, workdir: Path, export_format: str, params: Dict[str, Any]) -> Tuple[List[Path], int]:
    source, kind = _materialize_source(model, workdir)
    return _convert_from_source(
        script=_conversion_script(export_format),
        source=source,
        source_kind=kind,
        workdir=workdir,
        export_format=export_format,
        params=params,
        timeout=_export_timeout(export_format),
    )


def serialize_export(row) -> Dict[str, Any]:
    """Dual-key serialization: 前端 ModelExport 页面各处取的键名不一致（id/export_id、format/export_format 等）。"""
    created = row.created_at.isoformat() if row.created_at else None
    updated = row.updated_at.isoformat() if row.updated_at else None
    base = {
        'id': row.id,
        'exportId': row.id,
        'export_id': row.id,
        'model_id': row.model_id,
        'model_name': row.model_name,
        'model_path': row.model_path,
        'status': row.status,
        'error_message': row.error_message,
        'errorMessage': row.error_message,
        'size': row.size_bytes or 0,
        'size_bytes': row.size_bytes or 0,
        'export_path': row.export_path,
        'target_platform': row.target_platform,
        'quantized': bool(row.quantized),
        'imgsz': row.imgsz,
        'created_at': created,
        'export_time': updated or created,
    }
    return {
        **base,
        'format': row.export_format,
        'export_format': row.export_format,
    }


def list_exports(
    *,
    model_id: Optional[int] = None,
    export_format: Optional[str] = None,
    status: Optional[str] = None,
    search: str = '',
    page: int = 1,
    page_size: int = 10,
) -> Dict[str, Any]:
    from models import ModelExport

    query = ModelExport.query
    if model_id:
        query = query.filter(ModelExport.model_id == model_id)
    if export_format:
        query = query.filter(ModelExport.export_format == export_format.strip().lower())
    if status:
        query = query.filter(ModelExport.status == status.strip().upper())
    if search:
        query = query.filter(ModelExport.model_name.ilike(f'%{search.strip()}%'))

    total = query.count()
    page = max(1, int(page or 1))
    page_size = max(1, min(200, int(page_size or 10)))
    rows: List[Any] = (
        query.order_by(ModelExport.created_at.desc(), ModelExport.id.desc())
        .offset((page - 1) * page_size)
        .limit(page_size)
        .all()
    )
    return {'items': [serialize_export(r) for r in rows], 'total': total,
            'page': page, 'page_size': page_size}


def get_export(export_id: int):
    from models import ModelExport

    return ModelExport.query.get(export_id)


def delete_export(export_id: int) -> str:
    from models import db

    row = get_export(export_id)
    if row is None:
        raise ExportError(f'导出记录不存在: {export_id}')
    label = row.model_name or str(export_id)
    bucket, object_key = _resolve_bucket_key(row.export_path or '')
    if bucket and object_key:
        try:
            ModelService.delete_from_minio(bucket, object_key)
        except Exception as exc:  # noqa: BLE001 - record removal must not be blocked by storage
            logger.warning('删除导出对象失败 %s/%s: %s', bucket, object_key, exc)
    db.session.delete(row)
    db.session.commit()
    return label


def download_export(export_id: int) -> Tuple[str, str]:
    """Fetch the exported object into a temp file. Returns (tmp_path, download_name)."""
    row = get_export(export_id)
    if row is None:
        raise ExportError(f'导出记录不存在: {export_id}')
    if row.status != STATUS_COMPLETED:
        raise ExportError(f'导出任务尚未完成（当前状态 {row.status}）')
    bucket, object_key = _resolve_bucket_key(row.export_path or '')
    if not bucket or not object_key:
        raise ExportError('导出产物路径无法解析')

    ext = os.path.splitext(object_key)[1] or f".{EXT_BY_FORMAT.get(row.export_format or '', 'bin')}"
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=ext)
    tmp.close()
    ok, err = ModelService.download_from_minio(bucket, object_key, tmp.name)
    if not ok:
        os.unlink(tmp.name)
        raise ExportError(err or '导出产物下载失败')
    name = f'{row.model_name or "model"}_{row.model_id}_{row.export_format}{ext}'
    return tmp.name, name
