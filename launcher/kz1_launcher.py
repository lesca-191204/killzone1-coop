"""Killzone (PS2) co-op launcher: KillzoneRecomp fork, SCES-52004 disc.

The window is the Shadow Fall launcher's HTML page (web/, shown by pywebview in Edge WebView2) with Killzone 1 pages:
Inicio (disc image), Co-op (host / join over a virtual LAN), Ajustes (killzone.ini). The game is game/killzone.exe;
its settings stay in game/killzone.ini (the game's own file, edited line by line so comments and bindings survive).
"""
import base64
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import zipfile
import time
import webbrowser
from pathlib import Path

VERSION = '0.3.3'
DONATE_URL = 'https://www.paypal.com/paypalme/SantiagoLescano577'
DISCORD_URL = 'https://discord.gg/UYFcV5rbDw'
RADMIN_URL = 'https://www.radmin-vpn.com/'
NET_PORT = 47474

if getattr(sys, 'frozen', False):
    BASE = Path(sys.executable).resolve().parent
    WEB_DIR = Path(getattr(sys, '_MEIPASS', BASE)) / 'web'
else:
    BASE = Path(__file__).resolve().parent.parent if (Path(__file__).resolve().parent.parent / 'game').is_dir() \
        else Path(__file__).resolve().parent
    WEB_DIR = Path(__file__).resolve().parent / 'web'
GAME_DIR = Path(os.environ.get('KZ1_GAME_DIR') or BASE / 'game')
EXE = GAME_DIR / 'killzone.exe'
INI = GAME_DIR / 'killzone.ini'
COOP_DIR = BASE / 'coop'
LOG_DIR = BASE / 'logs'
SETTINGS = BASE / 'launcher.json'
UPDATE_DIR = BASE / 'update'
ART = BASE / 'launcher' / 'art.jpg'
ICON = BASE / 'launcher' / 'kz1.ico'
ICON_PNG = BASE / 'launcher' / 'kz1.png'

# Session frame rate (host's choice, sent to the joiner): game frames per second. 30 = the PS2's own pace.
FPS_CHOICES = [
    (30, '30 FPS', 'Como en PS2. Para PCs justas'),
    (45, '45 FPS', 'No recomendado: se desincroniza'),
    (60, '60 FPS', 'Recomendado'),
    (90, '90 FPS', 'Solo PCs muy potentes'),
]

CHARACTERS = [
    (0, 'Templar', 'Capitán Jan Templar'),
    (1, 'Luger', 'desde el capítulo 3'),
    (2, 'Rico', 'desde el capítulo 4'),
    (3, 'Hakha', 'desde el capítulo 5'),
]

TEXTS = {
    'home_sub': 'Campaña cooperativa online para 2 jugadores',
    'home_hint': 'Las dos PCs tienen que usar la misma ISO (española, SCES-52004) y esta misma versión del launcher.',
    'iso': 'Imagen del disco (ISO)',
    'iso_help': 'La ISO española de Killzone (SCES-52004). El co-op solo funciona con esa versión, igual en las dos PCs.',
    'browse': 'Elegir',
    'quick_links': 'Accesos',
    'nav_coop': 'Co-op online',
    'open_saves': 'Partidas guardadas',
    'open_logs': 'Registros (logs)',
    'theme': 'Tema',
    'support': 'Apoyar el proyecto',
    'donate': 'Donar por PayPal',
    'discord_join': 'Entrar al Discord',
    'play': 'JUGAR SOLO',
    'playing': 'EN JUEGO',
    'preparing': 'Juego abierto',
    'ok': 'Aceptar', 'cancel': 'Cancelar', 'yes': 'Sí', 'close': 'Cerrar', 'open_logs_q': 'Abrir logs',
    'more': 'Más', 'less': 'Menos', 'copy': 'Copiar', 'copied': 'Copiado', 'refresh': 'Actualizar',
    'get_radmin': 'Bajar Radmin VPN',
    'reset_all_confirm': '¿Volver todos los ajustes a los valores de fábrica?',
    'coop_intro': 'PROTOTIPO EXPERIMENTAL. Co-op por botones sincronizados: las dos PCs simulan exactamente la misma '
                  'partida, cuadro a cuadro, con los botones de los dos jugadores. El anfitrión es el jugador 1 y maneja '
                  'los menús; el invitado es el jugador 2 y ve lo mismo.',
    'coop_network': 'Tu conexión',
    'coop_net_help': 'Las dos PCs tienen que estar en la misma red virtual (Radmin VPN, recomendada; también Tailscale, '
                     'Hamachi o la misma red local). El invitado escribe la IP del anfitrión: la de Radmin empieza con 26. '
                     'El juego usa el puerto UDP 47474: la primera vez Windows pregunta por el firewall, aceptá en redes '
                     'privadas y públicas.',
    'coop_host': 'Crear partida',
    'coop_host_sub': 'Sos el jugador 1',
    'coop_host_help': 'Abre el juego y espera al jugador 2 (hasta 5 minutos; si no llega, jugás solo). Se usa TU partida '
                      'guardada (la misma que al jugar solo): niveles desbloqueados y progreso. El invitado la recibe al '
                      'conectarse. Pasale tu IP de arriba.',
    'update_available': 'Hay una versión nueva del co-op: v{version}.',
    'update_confirm': 'Se descarga la v{version} ({size}), se cierra el launcher, se copian los programas nuevos y se '
                      'vuelve a abrir. Tus ajustes, partidas e ISO no se tocan. El otro jugador tiene que tener la '
                      'misma versión.',
    'update_now': 'Actualizar ahora', 'update_notes': 'Notas', 'update_later': 'Después',
    'update_downloading': 'Descargando la actualización… {percent} %', 'update_unpacking': 'Preparando la actualización…',
    'update_failed': 'No se pudo actualizar: {error}', 'update_close_game': 'Cerrá el juego antes de actualizar.',
    'update_none': 'Tenés la última versión (v{version}).', 'update_check_failed': 'No se pudo buscar actualizaciones: {error}',
    'coop_fps': 'Cuadros por segundo de la partida',
    'coop_fps_help': 'Lo elige el anfitrión y el invitado juega con el mismo valor. Las dos PCs tienen que poder '
                     'sostenerlo: si una no llega, la partida entera va un poco más lenta (no se desincroniza). Si notás '
                     'cámara lenta, bajá a 30. Evitá 45: en la prueba del 07/11 se desincronizó.',
    'coop_host_button': 'Crear partida',
    'coop_join': 'Unirse',
    'coop_join_sub': 'Sos el jugador 2',
    'coop_join_help': 'Escribí la IP del anfitrión y conectate cuando él ya haya creado la partida.',
    'coop_char': 'Tu personaje',
    'coop_char_help': 'Se usa desde el capítulo en que ese protagonista aparece en la historia. Antes, por ahora, tenés '
                      'el mismo modelo que el jugador 1 (el soldado de la ISA no entra en la memoria de esos niveles).',
    'coop_join_button': 'Unirse',
    'coop_notes': 'A tener en cuenta',
    'coop_notes_help': '• Es un prototipo experimental: la campaña completa todavía no se probó entera.\n'
                       '• Problema conocido: al ganar un nivel y darle a Continuar, el paso al siguiente a veces se traba '
                       'o se cierra. Si pasa: cierren el juego los dos, vuelvan a entrar y elijan el nivel siguiente desde '
                       'el menú.\n'
                       '• En co-op se saltea el video de apertura del juego (rompe la sincronización); las cinemáticas de '
                       'los niveles se ven.\n'
                       '• Los FPS los elige el anfitrión. Si una PC no llega, la otra la espera: la partida va más lenta '
                       'pero no se desincroniza.\n'
                       '• Las dos PCs tienen que usar la misma versión del launcher.\n'
                       '• La tecla Tab / SELECT (lista de objetivos) está desactivada en co-op.\n'
                       '• Cada uno tiene sus propios gráficos, resolución y controles (Ajustes). En co-op el campo de '
                       'visión, la distancia de dibujo y los efectos quedan iguales en las dos PCs.\n'
                       '• Si un jugador muere, reaparece a los 5 segundos junto al otro. Si mueren los dos, se pierde la misión.\n'
                       '• Si alguien se desconecta, el otro sigue jugando solo.\n'
                       '• Si algo falla, guardá la carpeta logs de las dos PCs antes de volver a abrir el juego y '
                       'adjuntala en un issue de GitHub (los reportes van SOLO ahí y SIEMPRE con logs).',
    'need_iso': 'Elegí primero la ISO del juego (Inicio).',
    'need_ip': 'Escribí la IP del anfitrión.',
    'game_missing': 'Falta el juego (game\\killzone.exe). Volvé a descomprimir el paquete completo.',
    'game_ended': 'El juego se cerró con un error (código {code}). Los registros quedaron en la carpeta logs.',
    'iso_ok': 'Listo para jugar',
    'iso_missing': 'Elegí la ISO de Killzone',
    'iso_not_found': 'No se encuentra la ISO elegida',
    'iso_maybe': 'ISO elegida (el co-op necesita la española SCES-52004)',
}

# Settings of game/killzone.ini shown in Ajustes: (section, key, label, help, type, extra)
RESOLUTIONS = [('0x0', 'La del escritorio'), ('1280x720', '1280 × 720'), ('1600x900', '1600 × 900'),
               ('1920x1080', '1920 × 1080'), ('2560x1440', '2560 × 1440'), ('3840x2160', '3840 × 2160')]
INI_DEFAULTS = {
    'Display.WindowMode': 'borderless', 'Display.Resolution': '0x0', 'Display.VSync': '0', 'Display.ShowFps': '0',
    'Graphics.Renderer': 'd3d12', 'Graphics.Upscale': '4', 'Graphics.Anisotropy': '16', 'Graphics.SMAA': '1',
    'Graphics.FXAA': '0', 'Graphics.Sharpen': '30', 'Graphics.TextureReplacement': '1', 'Graphics.FieldOfView': '90',
    'Graphics.RenderDistance': '1', 'Input.MouseSensitivity': '3', 'Input.InvertY': '0', 'Input.RawMouse': '1',
    'Input.AimMode': 'Hold', 'Input.StickDeadzone': '0.15', 'Audio.MasterVolume': '100', 'Game.SkipIntro': '0',
}


def pages():
    def sel(key, label, options, help_=None):
        return {'type': 'select', 'key': key, 'label': label, 'options': options, 'help': help_}

    def tog(key, label, help_=None):
        return {'type': 'toggle', 'key': key, 'label': label, 'help': help_}

    def sld(key, label, lo, hi, step, decimals=0, unit='', help_=None):
        return {'type': 'slider', 'key': key, 'label': label, 'min': lo, 'max': hi, 'step': step, 'decimals': decimals,
                'unit': unit, 'help': help_, 'default': float(INI_DEFAULTS[key])}
    return [
        {'id': 'home', 'type': 'home', 'title': 'Inicio', 'icon': 'home'},
        {'id': 'coop', 'type': 'coop', 'title': 'Co-op online', 'icon': 'users', 'badge': 'BETA'},
        {'id': 'display', 'title': 'Pantalla y gráficos', 'icon': 'monitor', 'tabs': [
            {'id': 'screen', 'title': 'Pantalla', 'sections': [{'title': 'Ventana', 'items': [
                sel('Display.WindowMode', 'Modo', [['borderless', 'Pantalla completa sin bordes'],
                                                   ['fullscreen', 'Pantalla completa'], ['windowed', 'Ventana']]),
                sel('Display.Resolution', 'Resolución', [list(r) for r in RESOLUTIONS],
                    'Tamaño de la ventana. La imagen del juego se dibuja con la escala interna (Gráficos).'),
                tog('Display.VSync', 'Sincronización vertical', 'Evita cortes en la imagen; agrega un poco de demora.'),
                tog('Display.ShowFps', 'Mostrar FPS'),
            ]}]},
            {'id': 'gfx', 'title': 'Gráficos', 'sections': [{'title': 'Imagen', 'items': [
                sel('Graphics.Renderer', 'API gráfica', [['d3d12', 'Direct3D 12'], ['d3d11', 'Direct3D 11'],
                                                         ['vulkan', 'Vulkan']],
                    'Si el juego no abre o se ve mal, probá otra.'),
                sel('Graphics.Upscale', 'Escala interna', [[str(i), f'×{i}' + (' (nativa PS2)' if i == 1 else '')]
                                                           for i in range(1, 9)],
                    'Resolución a la que se dibuja el juego: ×4 ronda 1440p, ×6 4K. Más alto pide más GPU.'),
                sel('Graphics.Anisotropy', 'Filtro anisotrópico', [['0', 'Apagado'], ['2', '×2'], ['4', '×4'],
                                                                   ['8', '×8'], ['16', '×16']]),
                tog('Graphics.SMAA', 'Antialiasing SMAA'),
                tog('Graphics.FXAA', 'Antialiasing FXAA'),
                sld('Graphics.Sharpen', 'Nitidez', 0, 100, 5, unit='%'),
                tog('Graphics.TextureReplacement', 'Texturas mejoradas', 'Usa los paquetes de texturas si están instalados.'),
            ]}, {'title': 'Solo un jugador', 'subtitle': 'En co-op estos dos quedan fijos e iguales en las dos PCs',
                 'items': [
                     sld('Graphics.FieldOfView', 'Campo de visión', 60, 130, 1, unit='°'),
                     sld('Graphics.RenderDistance', 'Distancia de dibujo', 1, 4, 0.5, decimals=1, unit='×'),
                 ]}]},
        ]},
        {'id': 'controls', 'title': 'Controles y sonido', 'icon': 'gamepad', 'sections': [
            {'title': 'Mouse y teclado', 'items': [
                sld('Input.MouseSensitivity', 'Sensibilidad del mouse', 0.5, 10, 0.1, decimals=1),
                tog('Input.InvertY', 'Invertir eje vertical'),
                tog('Input.RawMouse', 'Mouse sin aceleración (raw)'),
                sel('Input.AimMode', 'Apuntar', [['Hold', 'Mantener'], ['Toggle', 'Alternar']]),
            ]},
            {'title': 'Joystick', 'items': [sld('Input.StickDeadzone', 'Zona muerta del stick', 0, 0.5, 0.01, decimals=2)]},
            {'title': 'Idioma', 'items': [
                sel('language', 'Idioma del juego', [['3', 'Español'], ['1', 'English'], ['2', 'Français'], ['4', 'Deutsch'],
                                                     ['5', 'Italiano'], ['6', 'Nederlands']],
                    'Textos, voces y videos. En co-op conviene que las dos PCs usen el mismo idioma.'),
            ]},
            {'title': 'Sonido y juego', 'items': [
                sld('Audio.MasterVolume', 'Volumen general', 0, 100, 1, unit='%'),
                tog('Game.SkipIntro', 'Saltear el video de introducción'),
            ]},
            {'type': 'help', 'title': None, 'items': [{'type': 'help', 'help':
                'Las teclas se cambian en game\\killzone.ini, sección [Bindings] (explicada en el mismo archivo).'}]},
        ], 'actions': [{'id': 'reset_all', 'label': 'Valores de fábrica'}]},
    ]


# -- killzone.ini -------------------------------------------------------------------------------------------------

def ini_read():
    values = {}
    try:
        section = ''
        for line in INI.read_text(encoding='utf-8', errors='replace').splitlines():
            s = line.strip()
            if s.startswith('[') and s.endswith(']'):
                section = s[1:-1]
            elif '=' in s and not s.startswith(';') and section != 'Bindings':
                k, v = s.split('=', 1)
                values.setdefault(f'{section}.{k.strip()}', v.strip())
    except OSError:
        pass
    return values


def ini_write(changes):
    """Sets section.key=value lines, keeping every other line (comments, bindings) as it is."""
    try:
        lines = INI.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        lines = []
    pending = dict(changes)
    section, out = '', []

    def flush(sec):
        for full, v in list(pending.items()):
            if full.split('.', 1)[0] == sec:
                out.append(f'{full.split(".", 1)[1]}={v}')
                del pending[full]
    for line in lines:
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            # keys of the section that ends that were not in the file
            while out and not out[-1].strip():
                out.pop()
            flush(section)
            if out:
                out.append('')
            section = s[1:-1]
            out.append(line)
            continue
        if '=' in s and not s.startswith(';') and section != 'Bindings':
            k = s.split('=', 1)[0].strip()
            full = f'{section}.{k}'
            if full in pending:
                out.append(f'{k}={pending.pop(full)}')
                continue
        out.append(line)
    flush(section)
    for full, v in pending.items():  # sections missing from the file
        sec, k = full.split('.', 1)
        out += ['', f'[{sec}]', f'{k}={v}']
    INI.parent.mkdir(parents=True, exist_ok=True)
    INI.write_text('\n'.join(out) + '\n', encoding='utf-8')


def ini_values():
    raw = ini_read()
    v = {}
    for key, default in INI_DEFAULTS.items():
        if key == 'Display.Resolution':
            w, hgt = raw.get('Display.Width', '0'), raw.get('Display.Height', '0')
            v[key] = f'{w}x{hgt}'
        else:
            v[key] = raw.get(key, default)
    return v


# -- launcher settings ----------------------------------------------------------------------------------------------

def load_settings():
    try:
        return json.loads(SETTINGS.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_settings(s):
    try:
        SETTINGS.write_text(json.dumps(s, indent=2, ensure_ascii=False), encoding='utf-8')
    except OSError:
        pass


def network_info():
    ips = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in [i['ip'] for i in ips] and not ip.startswith('127.') and not ip.startswith('169.254.'):
                kind = ('Radmin VPN' if ip.startswith('26.') else 'Hamachi' if ip.startswith('25.') else
                        'Tailscale' if re.match(r'^100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.', ip) else 'Red local')
                ips.append({'ip': ip, 'kind': kind})
    except OSError:
        pass
    order = {'Radmin VPN': 0, 'Tailscale': 1, 'Hamachi': 2, 'Red local': 3}
    ips.sort(key=lambda i: order[i['kind']])
    vpn = [i for i in ips if i['kind'] != 'Red local']
    if vpn:
        return {'level': 'ok', 'text': f'Red virtual {vpn[0]["kind"]} activa: tu IP para el invitado es {vpn[0]["ip"]}',
                'ips': ips}
    if ips:
        return {'level': 'warn', 'text': 'Sin red virtual: solo sirve si las dos PCs están en la misma red local. '
                                         'Para jugar por internet instalá Radmin VPN y entren a la misma red.', 'ips': ips}
    return {'level': 'error', 'text': 'Sin conexión de red', 'ips': ips}


# -- game -----------------------------------------------------------------------------------------------------------

def session_env(mode):
    """The settings every PC of a session must share (bats of the fork: KZ1 COOP - Anfitrion/Unirse)."""
    env = {
        'PS2X_WALLCLOCK': '0', 'PS2X_IOP_THREAD': '0', 'PS2X_VIF1_THREAD': '0', 'PS2X_DETERMINISTIC': '1',
        'KZ_AUDIO_PACE': '0', 'KZ_LOCKSTEP': '1',
        'PS2X_NO_PACING': '1', 'KZ_RENDER_DISTANCE': '1', 'KZ_LOD_SCALE': '1', 'KZ_SKIP_LOGOS': '1',
        'KZ_NET': mode,
        # Video (VIF1/VU1/GIF) on its own thread (11-05 night): the guest sees every DMA finish inside the store, as
        # the synchronous path, so the simulation is the synchronous one bit for bit (checked) while the worker draws.
        # Co-op went from ~29 to ~58 game frames/s on Santi's PC. The v0.1.2 freeze/crash (0x2b7adc) was the old
        # delayed-completion mode, not the thread.
        'PS2X_VIF1_THREAD': '1',
        # v0.3.2: a host-side crash (0xC0000005) prints the faulting function and stack into the log (11-05: the
        # host closed with 3221225477 at the start of level 2 while the joiner froze in a video DMA job).
        'KZ_CRASH_TRACE': '1',
        # v0.3.3 (11-07): Santi's real session (30 fps, 31 min) filled the 64 MB microVU1 cache 21 times (~6700
        # programs each); every reset recompiled for 1-2 s on both PCs, one after the other (the 1-2 s hitches and the
        # audio cuts). 256 MB = the most kz_vu accepts.
        'KZ_VU_CACHE_MB': '256',
    }
    # The opening intro movie is skipped in co-op again (v0.1.5): with it, Santi's v0.1.4 session desynced at game
    # frame 60 and the joiner's picture broke (movie decoding is not deterministic between PCs). Level cutscenes play.
    env['KZ_SKIP_INTRO'] = '1'
    # Diagnosis (0.2.3): full register/stack dump on a crash, and every movie lookup (name, found or not) in the log
    env['PS2X_MISSING_DUMP'] = '1'
    env['KZ_MOVIE_LOG'] = '1'
    return env


class Api:
    def __init__(self):
        self._window = None
        self._proc = None
        self._settings = load_settings()
        self._update = None
        self._updating = False

    def _emit(self, kind, data):
        if self._window is not None:
            try:
                self._window.evaluate_js(f'window.kz && window.kz.event({json.dumps(kind)}, {json.dumps(data)})')
            except Exception:  # noqa: BLE001
                pass

    # state ---------------------------------------------------------------------------------------------
    def _game(self):
        iso = self._settings.get('iso') or ini_read().get('Paths.ISO', '')
        if not EXE.is_file():
            return {'iso': iso, 'level': 'error', 'ok': False, 'message': TEXTS['game_missing']}
        if not iso:
            return {'iso': iso, 'level': 'warn', 'ok': False, 'message': TEXTS['iso_missing']}
        if not Path(iso).is_file():
            return {'iso': iso, 'level': 'error', 'ok': False, 'message': TEXTS['iso_not_found']}
        return {'iso': iso, 'level': 'ok', 'ok': True, 'message': TEXTS['iso_ok']}

    def coop_info(self):
        return {'network': network_info(), 'host_ip': self._settings.get('host_ip', ''),
                'character': self._settings.get('character', 0), 'characters': [list(c) for c in CHARACTERS],
                'fps': self._settings.get('session_fps', 60), 'fps_choices': [list(c) for c in FPS_CHOICES]}

    def get_state(self):
        values = ini_values()
        values['language'] = str(self._settings.get('language', '3'))
        art = ''
        if ART.is_file():
            art = 'data:image/jpeg;base64,' + base64.b64encode(ART.read_bytes()).decode()
        icon = ('data:image/png;base64,' + base64.b64encode(ICON_PNG.read_bytes()).decode()) if ICON_PNG.is_file() else art
        return {'texts': TEXTS, 'pages': pages(), 'values': values, 'game': self._game(), 'coop': self.coop_info(),
                'version': VERSION, 'lang': 'es', 'art': art, 'icon': icon, 'start_page': os.environ.get('KZ1_PAGE', 'home'), 'playing': self._proc is not None,
                'key_map': {}, 'pad_map': {}, 'reserved': []}

    def ready(self):
        self.check_updates(quiet=True)

    # updates (kz1_update.py: GitHub releases of kz1_update.REPO) ------------------------------------------
    def _update_view(self):
        u = self._update
        size = f"{u['size'] / (1 << 20):.0f} MB" if u.get('size') else '?'
        return {'text': TEXTS['update_available'].format(version=u['version']),
                'confirm': TEXTS['update_confirm'].format(version=u['version'], size=size)}

    def check_updates(self, quiet=False):
        import kz1_update

        def work():
            try:
                release, error = kz1_update.check(os.environ.get('KZ1_UPDATE_AS_VERSION') or VERSION), None
            except (OSError, ValueError) as e:
                release, error = None, e
            if release:
                self._update = release
                self._emit('update', self._update_view())
            elif not quiet:
                self._emit('message', {'text': TEXTS['update_check_failed'].format(error=error) if error
                                       else TEXTS['update_none'].format(version=VERSION)})
        threading.Thread(target=work, daemon=True).start()

    def install_update(self):
        import kz1_update
        release = self._update
        if not release:
            return {}
        if self._proc is not None:
            return {'error': TEXTS['update_close_game']}
        if not getattr(sys, 'frozen', False):  # running from source: the page has the zip
            webbrowser.open(release['page'])
            return {}
        self._updating = True

        def progress(done, total):
            self._emit('status', {'text': TEXTS['update_downloading'].format(percent=int(done * 100 / total) if total else 0),
                                  'busy': True})

        def work():
            try:
                archive = kz1_update.download(release, UPDATE_DIR, progress)
                self._emit('status', {'text': TEXTS['update_unpacking'], 'busy': True})
                unpacked = UPDATE_DIR / 'new'
                source = kz1_update.extract(archive, unpacked, Path(sys.executable).name)
                script = kz1_update.install_script(source, BASE, os.getpid(), Path(sys.executable).name, UPDATE_DIR,
                                                   cleanup=(unpacked, archive))
                kz1_update.run_script(script)
            except (OSError, ValueError, zipfile.BadZipFile) as e:
                self._updating = False
                self._emit('status', {'text': ''})
                self._emit('message', {'text': TEXTS['update_failed'].format(error=e)})
                return
            self._window.destroy()
            kz1_update.exit_for_update()
        threading.Thread(target=work, daemon=True).start()
        return {}

    def set(self, key, value):
        if key in ('character', 'language', 'session_fps'):
            self._settings[key] = value
            save_settings(self._settings)
            return value
        if isinstance(value, bool):
            value = '1' if value else '0'
        if isinstance(value, float) and value.is_integer() and key not in ('Graphics.RenderDistance',):
            value = int(value)
        value = str(value)
        if key == 'Display.Resolution':
            w, hgt = value.split('x')
            ini_write({'Display.Width': w, 'Display.Height': hgt})
        else:
            ini_write({key: value})
        return value

    def reset_all(self):
        changes = {k: v for k, v in INI_DEFAULTS.items() if k != 'Display.Resolution'}
        changes.update({'Display.Width': '0', 'Display.Height': '0'})
        ini_write(changes)
        return self.get_state()

    def choose_iso(self):
        import webview
        current = self._game()['iso']
        result = self._window.create_file_dialog(webview.FileDialog.OPEN, allow_multiple=False,
                                                 directory=str(Path(current).parent) if current else '',
                                                 file_types=('Imagen de disco (*.iso;*.bin;*.img)', 'Todos (*.*)'))
        if result:
            self._settings['iso'] = str(Path(result[0] if isinstance(result, (list, tuple)) else result))
            save_settings(self._settings)
            ini_write({'Paths.ISO': self._settings['iso']})
        return {'game': self._game()}

    def open(self, what):
        path = {'saves': GAME_DIR / 'disc_es' / 'mc0', 'logs': LOG_DIR}.get(what, BASE)
        path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    def open_url(self, what):
        if what == 'notes':
            import kz1_update
            webbrowser.open(self._update['page'] if self._update else kz1_update.RELEASES_PAGE)
            return
        webbrowser.open({'donate': DONATE_URL, 'discord': DISCORD_URL, 'radmin': RADMIN_URL}.get(what, DONATE_URL))

    # play ----------------------------------------------------------------------------------------------
    def _launch(self, env_extra, tag):
        game = self._game()
        if not game['ok']:
            return {'error': game['message'], 'page': 'home'}
        if self._proc is not None:
            return {'error': 'El juego ya está abierto.'}
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        # Keep the last 10 runs (host.log -> host.1.log -> ... -> host.10.log; same for *_net.log): before 0.2.1
        # every launch overwrote the previous log and only the last of several failed tries could be read. v0.3.3: 3 -> 10
        # (11-07: five tries in one evening, the first session lost its pair) and a date/time line at the top of each log.
        for stem in (tag, f'{tag}_net'):
            for i in range(10, 0, -1):
                src = LOG_DIR / (f'{stem}.log' if i == 1 else f'{stem}.{i - 1}.log')
                try:
                    if src.exists():
                        os.replace(src, LOG_DIR / f'{stem}.{i}.log')
                except OSError:
                    pass
        env = dict(os.environ)
        env['PS2X_LANGUAGE'] = str(self._settings.get('language', '3'))
        env.update(env_extra)
        args = [str(EXE), '--no-launcher', '--iso', game['iso']]
        log = open(LOG_DIR / f'{tag}.log', 'w', encoding='utf-8', errors='replace')
        log.write(f'[launcher] v{VERSION} {tag} {time.strftime("%Y-%m-%d %H:%M:%S")}\n')
        log.flush()
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        try:
            self._proc = subprocess.Popen(args, cwd=str(GAME_DIR), env=env, stdout=log, stderr=subprocess.STDOUT,
                                          creationflags=flags)
        except OSError as e:
            log.close()
            return {'error': f'No se pudo abrir el juego: {e}'}

        def wait(proc=self._proc):
            code = proc.wait()
            log.close()
            for name in ('killzone.log',):
                try:
                    shutil.copy2(GAME_DIR / name, LOG_DIR / f'{tag}-{name}')
                except OSError:
                    pass
            self._proc = None
            self._emit('playing', {'playing': False})
            self._emit('status', {'text': ''})
            if code not in (0, 1):
                self._emit('message', {'text': TEXTS['game_ended'].format(code=code), 'logs': True})
        threading.Thread(target=wait, daemon=True).start()
        return {'playing': True}

    def play(self):
        # launcher.json "all_chapters" (default false): every chapter can be picked in the campaign menu (Continue then
        # starts the last level, so it stays off unless asked)
        env = {'KZ_ALL_LEVELS': '1'} if self._settings.get('all_chapters', False) else {}
        return self._launch(env, 'solo')

    def coop_host(self):
        # The host's progress is the single-player memory card (disc_es\mc0, extracted with the boot files).
        mc = GAME_DIR / 'disc_es' / 'mc0'
        if not any(mc.glob('BESCES-52004*')) and (COOP_DIR / 'mc_template').is_dir():
            mc.mkdir(parents=True, exist_ok=True)
            shutil.copytree(COOP_DIR / 'mc_template', mc, dirs_exist_ok=True)
        env = session_env('host')
        # launcher.json "video_thread": false -> co-op without the video thread (upstream turns it off on the EU disc:
        # 'the worker's race corrupts memory'); A/B test for the 11-05 crashes/freezes. ~30 fps max then.
        if self._settings.get('video_thread', True) is False:
            env['PS2X_VIF1_THREAD'] = '0'
        env.update({'KZ_MC_ROOT': str(mc), 'KZ_NET_LOG': str(LOG_DIR / 'host_net.log'),
                    'KZ_NET_FPS': str(self._settings.get('session_fps', 60))})
        r = self._launch(env, 'host')
        if r.get('playing'):
            self._emit('status', {'text': 'Esperando al jugador 2 (puerto UDP 47474)…', 'busy': False})
        return r

    def coop_join(self, ip):
        ip = (ip or '').strip()
        if not ip:
            return {'error': TEXTS['need_ip']}
        self._settings['host_ip'] = ip
        save_settings(self._settings)
        mc = COOP_DIR / 'mc_join'
        mc.mkdir(parents=True, exist_ok=True)
        env = session_env('join')
        # launcher.json "video_thread": false -> co-op without the video thread (upstream turns it off on the EU disc:
        # 'the worker's race corrupts memory'); A/B test for the 11-05 crashes/freezes. ~30 fps max then.
        if self._settings.get('video_thread', True) is False:
            env['PS2X_VIF1_THREAD'] = '0'
        env.update({'KZ_MC_ROOT': str(mc), 'KZ_NET_ADDR': ip, 'KZ_COOP_CHAR': str(self._settings.get('character', 0)),
                    'KZ_NET_LOG': str(LOG_DIR / 'join_net.log')})
        r = self._launch(env, 'join')
        if r.get('playing'):
            self._emit('status', {'text': f'Conectando a {ip}…', 'busy': False})
        return r


def page_html():
    html = (WEB_DIR / 'index.html').read_text(encoding='utf-8')
    style = (WEB_DIR / 'style.css').read_text(encoding='utf-8')
    script = (WEB_DIR / 'app.js').read_text(encoding='utf-8')
    return html.replace('/*STYLE*/', style).replace('/*SCRIPT*/', script)


def dark_title_bar(window):
    try:
        import ctypes
        hwnd = window.native.Handle.ToInt32()
        value = ctypes.c_int(1)
        for attribute in (20, 19):
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(value), 4) == 0:
                break
    except Exception:  # noqa: BLE001
        pass


def main():
    import webview
    api = Api()
    scale = 1.0
    try:
        import ctypes
        scale = max(1.0, ctypes.windll.user32.GetDpiForSystem() / 96.0)
    except (AttributeError, OSError):
        pass
    bg = '#0c0b09'
    window = webview.create_window('Killzone Co-op', html=page_html(), js_api=api, width=int(1180 * scale),
                                   height=int(780 * scale), min_size=(int(960 * scale), int(640 * scale)),
                                   background_color=bg)
    api._window = window
    window.events.shown += lambda: dark_title_bar(window)
    webview.start(gui='edgechromium', debug=bool(os.environ.get('KZ1_WEB_DEBUG')), private_mode=True,
                  icon=str(ICON) if ICON.is_file() else None)


if __name__ == '__main__':
    main()
