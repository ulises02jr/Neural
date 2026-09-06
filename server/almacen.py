"""
Capa de almacenamiento de audio en DigitalOcean Spaces (compatible S3).

Los archivos pesados (pistas, tonos, proxys web, pads) viven en el bucket.
La metadata chica (canciones/*.json, secciones/midi/familias, config) queda LOCAL.

Se activa solo si hay credenciales spaces_* en secrets.json. Si no, habilitado()=False
y el servidor sigue usando el disco local (interruptor de seguridad).
"""
import json
import threading
from pathlib import Path

_SECRETS = Path(__file__).parent / "secrets.json"
_cfg = None
_client = None
_lock = threading.Lock()


def _load():
    global _cfg
    if _cfg is None:
        try:
            _cfg = json.loads(_SECRETS.read_text())
        except Exception:
            _cfg = {}
    return _cfg


def habilitado():
    """True si Spaces está configurado Y activado (spaces_activo != false)."""
    c = _load()
    if not (c.get("spaces_key") and c.get("spaces_secret") and c.get("spaces_bucket")):
        return False
    return c.get("spaces_activo", True) is not False


def _s3():
    global _client
    if _client is None:
        with _lock:
            if _client is None:
                import boto3
                from botocore.client import Config
                c = _load()
                _client = boto3.client(
                    "s3",
                    region_name=c["spaces_region"],
                    endpoint_url=c["spaces_endpoint"],
                    aws_access_key_id=c["spaces_key"],
                    aws_secret_access_key=c["spaces_secret"],
                    config=Config(signature_version="s3v4", retries={"max_attempts": 3}),
                )
    return _client


def _bucket():
    return _load()["spaces_bucket"]


def subir(key, local_path, content_type=None):
    extra = {"ContentType": content_type} if content_type else None
    _s3().upload_file(str(local_path), _bucket(), key, ExtraArgs=extra)


def subir_bytes(key, data, content_type=None):
    kw = {"Bucket": _bucket(), "Key": key, "Body": data}
    if content_type:
        kw["ContentType"] = content_type
    _s3().put_object(**kw)


def bajar(key, local_path):
    Path(local_path).parent.mkdir(parents=True, exist_ok=True)
    _s3().download_file(_bucket(), key, str(local_path))


def existe(key):
    try:
        _s3().head_object(Bucket=_bucket(), Key=key)
        return True
    except Exception:
        return False


def listar(prefix):
    """Lista todas las keys bajo un prefijo (maneja paginación)."""
    out, tok = [], None
    while True:
        kw = {"Bucket": _bucket(), "Prefix": prefix}
        if tok:
            kw["ContinuationToken"] = tok
        r = _s3().list_objects_v2(**kw)
        for o in r.get("Contents", []):
            out.append(o["Key"])
        if r.get("IsTruncated"):
            tok = r.get("NextContinuationToken")
        else:
            break
    return out


def listar_nombres(prefix):
    """Nombres de archivo (sin el prefijo) que cuelgan directo de un prefijo tipo carpeta."""
    p = prefix if prefix.endswith("/") else prefix + "/"
    nombres = []
    for k in listar(p):
        resto = k[len(p):]
        if resto and "/" not in resto:
            nombres.append(resto)
    return nombres


def borrar(key):
    try:
        _s3().delete_object(Bucket=_bucket(), Key=key)
    except Exception:
        pass


def borrar_prefijo(prefix):
    keys = listar(prefix)
    for i in range(0, len(keys), 1000):
        objs = [{"Key": k} for k in keys[i:i + 1000]]
        if objs:
            _s3().delete_objects(Bucket=_bucket(), Delete={"Objects": objs})


def url_firmada(key, expira=3600):
    """URL temporal (presigned) para descargar un objeto privado."""
    return _s3().generate_presigned_url(
        "get_object", Params={"Bucket": _bucket(), "Key": key}, ExpiresIn=expira)
