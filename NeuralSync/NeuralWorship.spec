# -*- mode: python ; coding: utf-8 -*-
# Empaquetado de NeuralWorship para Windows (PyInstaller, modo carpeta/onedir).
# Se ejecuta con:  pyinstaller --noconfirm NeuralWorship.spec
# (build_windows.bat copia antes transposicion.py y config.json)

block_cipher = None

# Recursos que van dentro del .exe (mismos que usa el motor)
datas = [
    ('templates', 'templates'),   # visor.html, panel.html
    ('static', 'static'),         # logo y assets del visor
    ('transposicion.py', '.'),       # motor de transposición
    ('config.json', '.'),            # config de fábrica (token; NO se sube al repo)
    ('icono_1024.png', '.'),         # ícono de la bandeja
]

# Módulos que PyInstaller no detecta solo (se cargan dinámicamente)
hiddenimports = [
    'transposicion',
    'rtmidi',
    'mido.backends.rtmidi',
    'webview.platforms.edgechromium',
    'webview.platforms.winforms',
    'pystray._win32',
    'clr',
]

a = Analysis(
    ['app_windows.py'],
    pathex=['.'],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='NeuralWorship',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,          # sin ventana negra de consola
    disable_windowed_traceback=False,
    icon='icono.ico',
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='NeuralWorship',
)
