> 🎮 **Interested in my work? Check out my portfolio → [lesca-191204.github.io](https://lesca-191204.github.io)**<br>
> ¿Te interesa mi trabajo? Mirá mi portfolio.

# Killzone Native Online CO-OP Campaign

**English** | [Español](#español)

[![Join the Ports Discord](https://img.shields.io/badge/Discord-Join%20the%20Ports%20Discord-5865F2?logo=discord&logoColor=white)](https://discord.gg/UYFcV5rbDw)
[![Support the project and future ports](https://img.shields.io/badge/PayPal-Support%20the%20project%20%26%20future%20ports-00457C?logo=paypal&logoColor=white)](https://www.paypal.com/paypalme/SantiagoLescano577)
[![Latest release](https://img.shields.io/github/v/release/lesca-191204/killzone1-coop?include_prereleases&label=Download&color=2ea44f)](../../releases)

> ⚠️ **EXPERIMENTAL PROTOTYPE, in development.** The goal is simple: play the **whole Killzone (PS2, 2004)
> campaign in online co-op**, two players, natively on PC. Today the first levels play well, but **the full
> campaign has not been played through yet** and there is a known problem when moving from one level to the
> next (see [Known issues](#-known-issues)). Support is limited: it is a hobby project. **Every report with
> logs helps finish it faster.**

Killzone on PS2 never had a co-op campaign. This project adds one: **two players over the internet**, each on
their own PC, playing the same campaign together. It uses the game's **own multiplayer code** (the split-screen
*Battlefields* mode): player 2 is created by the game itself, at the same moment as player 1, with its own
controls, camera, weapons and respawn.

> 🧱 **Base:** the native PC port is **[KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp) by
> [drakolordx7](https://github.com/drakolordx7)** (the game's code recompiled to C++ with
> [PS2Recomp](https://github.com/ran-j/PS2Recomp)). **Everything about co-op is ours:** the network code, the
> deterministic simulation, player 2 inside the campaign, Spanish disc support, the launcher, and the fixes to
> the runtime that co-op needed. We'll keep improving both the co-op and the game itself.

> ⚠️ **No game files are included.** You need your own copy of the game: the **Spanish disc ISO
> (SCES-52004)**, the same on both PCs. This project is not affiliated with Sony Interactive Entertainment or
> Guerrilla Games. Killzone is a trademark of Sony Interactive Entertainment.

## 💿 Required game version (only this one)

| | |
|---|---|
| ✅ **Supported** | **Killzone, Spanish/European disc `SCES-52004`** (PAL, 6 languages), as an ISO image |
| ❌ Not supported | US disc `SCUS-97402`, the other PAL disc `SCES-52893`, any other release, Platinum or demo |

- **Co-op only works with `SCES-52004`, and both PCs need it.** The co-op code is tied to that exact game
  executable. The launcher tells you if the chosen ISO is a different version.
- **There are no plans to support other versions** at the moment.
- **Language:** the disc has 6 languages (Español, English, Français, Deutsch, Italiano, Nederlands). Change it in
  the launcher → **Controles y sonido → Idioma** (texts, voices and videos). **In co-op, both PCs should use the
  same language** (voices and videos are part of what both PCs simulate).

## 🎯 Scope: what this project is (and isn't)

- **The goal is one thing: play the complete Killzone campaign in online co-op, from start to finish.** That's it.
- It is **not** a general port project: no new game modes, no versus/online multiplayer revival, no other
  versions or consoles for now. Please don't open requests for those.
- Development continues **depending on the support and interest** it gets (reports, testing, donations).
  Support is limited and given in free time.

## 📥 Download and play

1. Download the latest `KZ1-Coop-v….zip` from **[Releases](../../releases)** and unzip it anywhere (outside
   "Program Files"). **Both players need the same version.**
2. Open **KZ1 Coop.exe** and choose your **Spanish ISO (SCES-52004)** on the *Home* page.
3. Put both PCs on the same virtual network: **[Radmin VPN](https://www.radmin-vpn.com/)** is recommended
   (free). One player creates a network, the other joins it with its name and password. Tailscale, Hamachi or
   the same local network also work.
4. Go to **Co-op online**:
   - **Host (player 1):** choose the frame rate and press **Create game**. Send your Radmin IP (starts with
     `26.`) to the other player. The game opens and waits for player 2 (up to 5 minutes).
   - **Guest (player 2):** choose your character, type the host's IP and press **Join**.
   - The first time, Windows asks about the firewall: **allow** it on private and public networks (UDP port
     `47474`).
5. The host drives the menus (level, difficulty, pause); the guest sees the same screens.

The launcher updates itself when a new version is published. **JUGAR SOLO** (Play solo) starts the normal
single-player game.

| | Requirements |
|---|---|
| 🪟 System | Windows 10 or 11, 64-bit |
| 🧠 CPU | Any x64 CPU with SSE4.1; AVX2 CPUs get the faster code automatically. Co-op needs more CPU than solo |
| 💿 Game | Killzone **Spanish disc (SCES-52004)** ISO, identical on both PCs |
| 🌐 Network | Both PCs on the same virtual network (Radmin VPN recommended) |

## 🎞️ Frame rate (FPS): how it works

Both PCs **run exactly the same game, frame by frame**: only the two players' buttons travel over the network,
and every PC simulates the whole match with them (lockstep). That is why it stays in sync without a server.

- The **host chooses the FPS** (30, 45, **60 recommended**, 90) and the guest plays at the same value.
- **Both PCs must be able to hold it.** If one PC falls behind, the other waits for it: the whole game runs a bit
  slower (slow motion), but it **never desyncs**. If you notice slow motion, lower it to 45 or 30.
- Each player keeps their own resolution, graphics and controls (launcher *Settings*). In co-op the field of
  view, draw distance and effects are forced equal on both PCs, because they affect the simulation.

## 🐞 Known issues

- **Level change:** after winning a level and pressing *Continue*, the move to the next level sometimes hangs or
  closes the game. **Workaround:** both players close the game, open it again and choose the next level from
  the menu. Being investigated (it looks related to the video thread of the runtime).
- **Player 2's character:** the character you choose is used from the chapter where that hero joins the story
  (Templar and Luger from chapter 3, Rico from 4, Hakha from 5). Before that, player 2 uses the same model as
  player 1 for now (the generic ISA soldier does not fit in the memory of those levels).
- The opening intro video of the game is skipped in co-op (it breaks the sync). **Level cutscenes play.**
- **Tab / SELECT** (objectives screen) is disabled in co-op: it froze the session.

## 🧾 Reporting problems

> ❗ **Bugs are reported ONLY in [GitHub Issues](../../issues), and ALWAYS with logs.** Reports on Discord or
> without logs are not taken.

If something fails, **save the `logs` folder of BOTH PCs before opening the game again** (only the last 3
sessions are kept), zip it and attach it to the issue. Say what you were doing, which level and which FPS.

## ⚙️ How it works (short)

- **Lockstep netcode** (`src/kz_netplay.cpp`, `src/kz_lockstep.cpp`): UDP, buttons and mouse per frame, input
  delay, a digest of the game state every 600 frames to detect any desync.
- **Deterministic simulation:** the PS2 runtime runs on a fixed virtual clock (EE, IOP and VIF1 timing), so two
  PCs produce bit-identical games. The video thread completes every DMA instantly from the game's point of view.
- **Native player 2** (`src/kz_coop.cpp`): the player slot is configured like the game's *Battlefields* menu does
  ("Human" control type) and player 2 is added with the game's own `AddPlayer` right after the level's
  `OnInitialize`, before the level-start checkpoint. Death, respawn and checkpoints use the game's own logic.
- **Spanish disc support:** a third recompiled set (SCES-52004) next to the US and PAL ones.

More detail: [docs/coop.md](docs/coop.md).

## 🔧 Building from source

Same as KillzoneRecomp ([docs/README-upstream.md](docs/README-upstream.md)), plus:

- The runtime is our fork **[lesca-191204/PS2Recomp](https://github.com/lesca-191204/PS2Recomp/tree/killzone-coop)**,
  branch `killzone-coop` (submodule `ext/PS2Recomp`).
- The Spanish code set is generated from your own disc's ELF: `python tools/scripts/regen.py --es`
  (`config/killzone_es.toml` → `generated_es/`, not included).
- PCSX2 (`ext/pcsx2`, commit `646df006a4`) needs `patches/pcsx2/*.patch` (`git apply` inside `ext/pcsx2`).
- Build with `tools\scripts\build.bat`. The launcher (`launcher/`) is packaged with `launcher/package.sh`
  (PyInstaller + pywebview).

## 🙏 Credits

- **[drakolordx7](https://github.com/drakolordx7)**: KillzoneRecomp, the native PC port this is built on.
- **[ran-j](https://github.com/ran-j)**: PS2Recomp.
- PCSX2 (graphics, sound), SDL3, Dear ImGui, FFmpeg and the other third-party parts listed in the upstream README.
- Co-op, launcher and Spanish disc support: **[lesca-191204](https://github.com/lesca-191204)** (Santiago Lescano).

## 📜 License

GPL-3.0, like KillzoneRecomp (see [LICENSE](LICENSE)).

---

# Español

> ⚠️ **PROTOTIPO EXPERIMENTAL, en desarrollo.** El objetivo es simple: jugar **la campaña completa de Killzone
> (PS2, 2004) en cooperativo online**, dos jugadores, nativo en PC. Hoy los primeros niveles se juegan bien,
> pero **la campaña completa todavía no se probó entera** y hay un problema conocido al pasar de un nivel al
> siguiente (ver [Problemas conocidos](#-problemas-conocidos)). El soporte es limitado: es un proyecto hobby.
> **Cada reporte con logs ayuda a terminarlo más rápido.**

Killzone de PS2 nunca tuvo campaña cooperativa. Este proyecto se la agrega: **dos jugadores por internet**,
cada uno en su PC, jugando juntos la misma campaña. Usa **el propio código multijugador del juego** (el modo
*Battlefields* a pantalla dividida): el jugador 2 lo crea el juego mismo, en el mismo momento que el jugador 1,
con sus propios controles, cámara, armas y reaparición.

> 🧱 **Base:** el port nativo para PC es **[KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp) de
> [drakolordx7](https://github.com/drakolordx7)** (el código del juego recompilado a C++ con
> [PS2Recomp](https://github.com/ran-j/PS2Recomp)). **Todo lo del co-op es nuestro:** la red, la simulación
> determinista, el jugador 2 dentro de la campaña, el soporte del disco español, el launcher y los arreglos al
> runtime que hicieron falta. Vamos a seguir mejorando el co-op y el juego en sí.

> ⚠️ **No incluye archivos del juego.** Necesitás tu propia copia: la **ISO del disco español (SCES-52004)**,
> la misma en las dos PCs. Este proyecto no está afiliado a Sony Interactive Entertainment ni a Guerrilla Games.

## 💿 Versión del juego necesaria (solo esta)

| | |
|---|---|
| ✅ **Compatible** | **Killzone, disco español/europeo `SCES-52004`** (PAL, 6 idiomas), como imagen ISO |
| ❌ No compatible | Disco de EE. UU. `SCUS-97402`, el otro disco PAL `SCES-52893`, cualquier otra edición, Platinum o demo |

- **El co-op funciona solo con `SCES-52004`, y la necesitan las dos PCs.** El código del co-op está atado a ese
  ejecutable exacto del juego. El launcher te avisa si la ISO elegida es otra versión.
- **No hay planes de soportar otras versiones** por ahora.
- **Idioma:** el disco trae 6 idiomas (Español, English, Français, Deutsch, Italiano, Nederlands). Se cambia en el
  launcher → **Controles y sonido → Idioma** (textos, voces y videos). **En co-op las dos PCs tienen que usar el
  mismo idioma** (las voces y los videos son parte de lo que simulan las dos PCs).

## 🎯 Alcance: qué es este proyecto (y qué no)

- **El objetivo es uno solo: jugar la campaña completa de Killzone en cooperativo online, de principio a fin.**
  Nada más.
- **No** es un proyecto de port general: no hay modos nuevos, ni revivir el multijugador online/versus, ni otras
  versiones o consolas por ahora. Por favor no abran pedidos de eso.
- El desarrollo sigue **según el apoyo y el interés** que tenga (reportes, pruebas, donaciones). El soporte es
  limitado y en tiempo libre.

## 📥 Descargar y jugar

1. Bajá el último `KZ1-Coop-v….zip` de **[Releases](../../releases)** y descomprimilo en cualquier carpeta
   (fuera de "Archivos de programa"). **Los dos jugadores necesitan la misma versión.**
2. Abrí **KZ1 Coop.exe** y elegí tu **ISO española (SCES-52004)** en *Inicio*.
3. Poné las dos PCs en la misma red virtual: **[Radmin VPN](https://www.radmin-vpn.com/)** (gratis,
   recomendada). Uno crea una red y el otro entra con el nombre y la contraseña. También sirven Tailscale,
   Hamachi o la misma red local.
4. Andá a **Co-op online**:
   - **Anfitrión (jugador 1):** elegí los FPS y tocá **Crear partida**. Pasale tu IP de Radmin (empieza con
     `26.`) al otro. El juego se abre y espera al jugador 2 (hasta 5 minutos).
   - **Invitado (jugador 2):** elegí tu personaje, escribí la IP del anfitrión y tocá **Unirse**.
   - La primera vez Windows pregunta por el firewall: **permitilo** en redes privadas y públicas (puerto UDP
     `47474`).
5. El anfitrión maneja los menús (nivel, dificultad, pausa); el invitado ve lo mismo.

La partida guardada que se usa es la del anfitrión (la misma de su modo solo): niveles desbloqueados y
progreso. El launcher se actualiza solo cuando sale una versión nueva. **JUGAR SOLO** abre el juego normal de
un jugador.

## 🎞️ FPS: cómo funciona

Las dos PCs **simulan exactamente la misma partida, cuadro a cuadro**: por la red viajan solo los botones de los
dos jugadores y cada PC calcula la partida entera con ellos (lockstep). Por eso se mantiene sincronizado sin
servidor.

- **Los FPS los elige el anfitrión** (30, 45, **60 recomendado**, 90) y el invitado juega con el mismo valor.
- **Las dos PCs tienen que poder sostenerlo.** Si una se atrasa, la otra la espera: la partida entera va más
  lenta (cámara lenta), pero **nunca se desincroniza**. Si notás cámara lenta, bajá a 45 o 30.
- Cada uno tiene su resolución, gráficos y controles (*Ajustes* del launcher). En co-op el campo de visión, la
  distancia de dibujo y los efectos quedan iguales en las dos PCs, porque afectan a la simulación.

## 🐞 Problemas conocidos

- **Cambio de nivel:** al ganar un nivel y darle a *Continuar*, el paso al siguiente a veces se traba o cierra el
  juego. **Solución:** cierren el juego los dos, vuelvan a entrar y elijan el nivel siguiente desde el menú. Lo
  estamos investigando (parece relacionado con el hilo de video del runtime).
- **Personaje del jugador 2:** el que elegís se usa desde el capítulo en que ese protagonista entra en la
  historia (Templar y Luger desde el 3, Rico desde el 4, Hakha desde el 5). Antes, por ahora, el jugador 2 usa
  el mismo modelo que el jugador 1 (el soldado genérico de la ISA no entra en la memoria de esos niveles).
- En co-op se saltea el video de apertura del juego (rompe la sincronización). **Las cinemáticas de los niveles
  se ven.**
- **Tab / SELECT** (pantalla de objetivos) está desactivado en co-op: trababa la partida.

## 🧾 Reportar problemas

> ❗ **Los problemas se reportan SOLO en [GitHub Issues](../../issues), y SIEMPRE con logs.** Lo que llegue por
> Discord o sin logs no se toma.

Si algo falla, **guardá la carpeta `logs` de LAS DOS PCs antes de volver a abrir el juego** (solo se guardan
las últimas 3 partidas), comprimila y adjuntala al issue. Contá qué estaban haciendo, en qué nivel y con cuántos FPS.

## 🙏 Créditos

- **[drakolordx7](https://github.com/drakolordx7)**: KillzoneRecomp, el port nativo sobre el que se construye esto.
- **[ran-j](https://github.com/ran-j)**: PS2Recomp.
- Co-op, launcher y soporte del disco español: **[lesca-191204](https://github.com/lesca-191204)** (Santiago Lescano).

Licencia GPL-3.0, igual que KillzoneRecomp.
