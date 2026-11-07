# Changelog

Every ELF is `dist/AnyPad-PS5-<version>.elf`, with a `.sha256` beside it. The
version is set in `src/version.h` and is also in the first line of the log, in
the start-up notification, on the web page, and in the binary
(`strings AnyPad-PS5-*.elf | grep anypad-version`).

## Unreleased

- Fix a shutdown use-after-free: stopping the web server now clears its owning pointer before Bluetooth cleanup can call the web-serving idle hook. Add a connected-controller shutdown regression test.

## 0.5.5-beta

- Primera beta. Base: 0.4.7-alpha, que encuentra el DS4 en la consola; la 0.5.4 (= 0.4.7 + botón «Pulsar PS») se confirmó que funciona y pasa a beta.
- El botón **«Pulsar PS»** (azul) se mueve a «Mandos emparejados», a la izquierda de «Olvidar», y aparece en los mandos conectados. Solo hace falta con mandos que no tienen botón PS (los que no son de PlayStation).
- Textos de la web y del README: al pulsarlo la consola muestra un mensaje del sistema que se refiere al DualSense (recuperar la prioridad); con un mando que no es de PlayStation la prioridad vuelve al DualSense, y para devolver el control al mando emparejado hay que volver al menú y pulsar «Pulsar PS» otra vez.
- Cambio quirúrgico: solo `web/index.html` (y la versión); la búsqueda y el Bluetooth no se tocan.

## 0.5.4-alpha

- Parte de la 0.4.7 (la que encuentra el DS4 en la consola del usuario) y añade SOLO el botón «Pulsar PS» por mando y los hasta 4 mandos. Se descartan los cambios de la búsqueda de mandos de 0.5.1/0.5.2 (vigilante de 14 s, cancelar al arrancar/parar, esperar ante 0x0c), porque con la 0.5.2 el DS4 dejó de encontrarse y con la 0.4.7 sí.
- Esos cambios pueden volver a probarse de uno en uno, solo si el usuario lo pide.

## 0.5.3-alpha

- Se ELIMINA el botón «Reiniciar Bluetooth» y el `HCI_Reset` (0.5.2): al probarlo en la consola el usuario se quedó sin DualSense; reiniciar el controlador del chip es peligroso. No se vuelve a añadir.
- Contiene lo de 0.5.0 (botón «Pulsar PS» por mando, hasta 4 mandos) y 0.5.1 (la búsqueda no se atasca, limpieza de búsqueda y enlaces huérfanos al arrancar).

## 0.5.1-alpha

- La búsqueda de mandos ya no se queda atascada: si el aviso de "fin de búsqueda" del chip se pierde, a los 14 s se cancela y se busca otra vez (antes quedaba parada hasta 45 s, y el mando "no aparecía").
- Al arrancar se cancela cualquier búsqueda que una ejecución anterior cortada dejara en el chip; si el chip contesta "comando no permitido" (0x0c) se espera a que acabe en vez de repetir la orden decenas de veces; al parar AnyPad no se deja ninguna búsqueda en marcha.
- Un aviso de fin de búsqueda dañado (longitud errónea) ya no se toma por bueno.
- Test nuevo `test_inquiry_watchdog` (probado con mutación).

## 0.5.0-alpha

- Base: 0.4.7-alpha (se descarta el cambio de Select como PS de la 0.4.8).
- Botón **«Pulsar PS»** en la tarjeta de cada mando del menú web: equivale a mantener pulsado PS en ese mando (envía PS al mando virtual durante 700 ms). Sirve para activar uno u otro mando como principal. Nuevo `POST /api/ps?slot=N`.
- Hasta 4 mandos a la vez (límite de `HOST_MAX_PADS`), documentado; la web explica cómo nombrar los usuarios de la consola según el mando.
- README: recomendación de nombrar el usuario como el mando, varios mandos, "igual en cualquier consola", cuenta de X @elmonomalvad0.

## 0.4.7-alpha

- Texto corregido sobre el usuario creado: se juega con el usuario principal y se pulsa PS un momento para usar el mando del usuario creado (notificación de arranque, web, README, notas de la release).
- README: se indica que se puede añadir al autoload; los mandos de otras consolas se presentan como "soportados para usar en la PS5, aún sin probar".

## 0.4.6-alpha

- Aviso de arranque y página web: hay que crear un usuario nuevo en la consola y asignarle el mando cuando lo detecte; con el usuario del DualSense los juegos no ven el mando. README actualizado.
- Nombre: desde esta versión los ELF se llaman `-alpha` en vez de `-experimental`.
- El log registra al arrancar el firmware de la consola (`console: firmware 10.01`) y los procesos que no son del sistema (otros payloads en marcha), para comparar consolas y firmwares.
- La web: el botón «Salir» solo cierra la página (AnyPad sigue funcionando); apagar AnyPad queda como botón «avanzado» aparte.
- Nuevo `COMPATIBILITY.md` (castellano e inglés): solo el DualShock 4 está verificado en consola.

## 0.4.5-experimental

- Base: 0.4.1 (se retiran de HEAD los cambios de 0.4.2/0.4.3; siguen en el historial) + `bind_user` de 0.4.4.
- Nuevo: si al paginar un mando el chip contesta "la conexión ya existe" (0x0b), se cierran los enlaces huérfanos de una ejecución anterior y se vuelve a buscar. Causa de que "se corrompa" tras relanzar el ELF sin parar antes.

## 0.4.4-experimental

- Construida sobre la **0.4.1** (la que encuentra el DS4), no sobre 0.4.2/0.4.3, más un único cambio: `/data/anypad/bind_user` (opcional) decide a qué usuario se da el mando virtual: `other` (un usuario con sesión que no sea el principal) o un id en hexadecimal. Sin el fichero todo es igual que la 0.4.1. Para probar si un juego lanzado por ese otro usuario recibe el mando.
- El log `users:` por fin se imprime cuando cambia algo (antes un detector erróneo no lo mostraba).

## 0.4.3-experimental

- Diagnóstico: cada 10 s, si llegó algo, el log muestra `usb: stats: N pieces, M events, D dropped, ACL K; types xx:n ...` (tipos de evento HCI). Sirve para ver si el chip responde durante la búsqueda cuando "no encuentra el mando" (0x02 = resultado de inquiry, 0x22 con RSSI, 0x2f extendido, 0x03 conexión completa, 0x0f estado de comando).

## 0.4.2-experimental

- Reintentos de paginación al mando (hasta 3) si falla la conexión durante el emparejamiento.
- El DS4 recibe antes su luz azul: el reintento de la secuencia de arranque pasa de 800 a 250 ms (hasta 12 rondas).
- Se quita de la web el aviso "Menú en la consola" (era una obviedad).
- Reensamblado de eventos: una pieza corta cierra el evento (evita que una pieza perdida estropee el siguiente).

## 0.4.1-experimental

- Prueba 6 (0.4.0): el DS4 se encuentra, conecta, se empareja y cifra el enlace, pero la escritura ACL por el endpoint 0x01 nunca termina ('ACL out still busy'), por lo que L2CAP no recibe respuesta. Si no termina en 0,5 s, se para y se usa el segundo bulk OUT de la función (0x02), y se registra 'usb: first ACL packet written on endpoint ...'.
- Se quita el atajo de mando (no se podía leer el DualSense desde un payload): el menú se abre desde Contenido multimedia o por la web.

## 0.4.0-experimental

- Acceso en "Contenido multimedia" (ANYP00001, "AnyPad", icono del mando naranja) que abre
  `http://127.0.0.1:8095/` en el navegador, con el mismo mecanismo que Payload Manager
  (`/user/app/ANYP00001/sce_sys/{param.json,icon0.png}` + `sceAppInstUtilAppInstallTitleDir`).
  Se instala al arrancar; `/data/anypad/no_icon` lo evita y `/data/anypad/remove_icon` lo desinstala.
  Icono: `assets/icon0.png` (512x512 PNG con transparencia, negro bajo los píxeles transparentes),
  comprobado con `tools/check-icon.py`.
- Combinación de la consola por defecto: **L2 + R2 + OPTIONS** (1,5 s).
- Búsqueda de mandos con trazas ('inquiry: started / controller answered status / class ... ignored / finished')
  porque en la prueba 0.3.4 "Emparejar" no dejaba rastro alguno.

## 0.3.4-experimental (3 October 2026)

Made after the fourth run on a console (0.3.3), which dumped the Bluetooth chip's
USB configuration for the first time and explained the "silent" chip.

- **Fixed: the Bluetooth chip was answering and the answer was thrown away.** It
  answered `Read Buffer Size` with a valid *Command Complete* event
  (`0e 0b 01 05 10 00 fd 03 f0 08 00 08 00`) on endpoint `0x81`, which the code,
  following an assumed layout, treated as the ACL pipe. On this console
  (PS5 fat, firmware 10.01) the layout is the standard one: **events `0x81`
  (interrupt), ACL in `0x82`, ACL out `0x01`.** The endpoints are now read from the
  chip's own configuration descriptor (`usb_desc.c`, tested against the real
  dump in `tests/fixtures/`), and commands go to the interface of the chosen
  function. Events, which arrive in 16-byte pieces, are reassembled
  (`evstream.c`).
- **Fixed: the menu answered `{"error":"busy"}` after pressing the Bluetooth
  button.** Each Bluetooth try blocked the program for ~5 s without serving the
  web, the browser piled up connections and the 4 slots filled. The host now calls
  an idle hook whenever it waits, which serves the web; 8 slots.
- **Fixed: the console hotkey could not open the pad** (`scePadOpen` answered
  `0x809b0081`, `USER_NOT_LOGIN`). The user was taken from the foreground; now it
  is picked from the signed-in users (the initial one if signed in, then the
  foreground one, then the first), looked for again every 5 s if there was none
  or the system says it is not signed in. Virtual pads use the same choice.
- Logging: the pad-open and hotkey lines are written when the answer changes, not
  every 5 s.

## 0.3.3-experimental (3 October 2026)

Made after the third run on a console (0.3.2): the menu worked, the console
hotkey did not, and the Bluetooth chip still did not answer. See PRUEBAS.md.

- **Fixed: the console hotkey could not read the user's pad.** `scePadGetHandle`
  answered `0x80920008` (NO_HANDLE): a payload has no handle for the user's pad
  until it opens it. The pad is now opened the normal way (`scePadOpen`, as any
  application does; at most one try every 5 s, logged) and closed on exit.
- **Fixed: a stale instance lock survived a reboot.** The lock held only a
  process id, which means nothing after a reboot, so a leftover lock could look
  like a running copy. It now also holds the console's boot time, and a lock from
  another boot, or whose process is gone, is taken over (`lock.c`, `test_lock`).
- **More Bluetooth diagnosis.** The `diag:` lines now also dump the device's
  whole configuration descriptor (every interface and endpoint), the kernel driver
  holding each interface, the device's name and state, and the first read that
  finished (which endpoint, how many bytes, which bytes). Read-only; nothing is
  sent to or taken from the chip for this.
- The 0.3.2 result that shaped this: the credentials are **not** what stops the
  chip from answering (same silence with the original ones), and only one read
  finished in five seconds, with no event in it.

## 0.3.2-experimental (3 October 2026)

Made after the second run on a console (see PRUEBAS.md): the virtual-pad
start-up now passed, but the Bluetooth chip did not answer, and the menu never
appeared because it was started after the Bluetooth.

- **The menu starts first.** The web page is up before anything that can fail, so
  it is there to explain a failure. If the virtual pads or the Bluetooth are not
  available, AnyPad PS5 **stays running** in a reduced mode (menu, log, stop
  and a *Reintentar Bluetooth* button) instead of exiting.
- **The menu opens on the console with a button combination:** hold
  **L1 + R1 + touchpad for 1.5 s** on the DualSense and the console's browser
  opens `http://127.0.0.1:8095/` (`sceSystemServiceLaunchWebBrowser`, as Payload
  Manager does). Configurable in `/data/anypad/config.ini` (`menu_combo`,
  `menu_hold_ms`). The menu says whether the user's pad can be read and why not.
- **The start-up notification says where the menu is and how to open it:** the
  console's address (`http://<ip>:8095/`) and the combination, in Spanish.
- **Bluetooth diagnosis.** When the chip does not answer, the transport logs
  what it knows (`diag:` lines): the USB device, how many reads finished and with
  which status, how many events and ACL packets were seen, the first events in
  hexadecimal, and the last command sent.
- **Credentials experiment.** The first two Bluetooth tries are made with the
  raised credentials; if the chip is silent, they are put back and three more
  tries are made. The log says which worked. Virtual pads raise them again when
  needed. (Whether the raised credentials stop the chip from answering is not
  known.)
- After 5 failed tries (bounded) it stops trying by itself; the menu's *Reintentar
  Bluetooth* button tries again. Losing the Bluetooth later no longer stops the
  program: it reports it and offers the same button.
- The pairing key combination (L1+R1+Square) is gone; pair from the menu or with
  the `/data/anypad/pair` file.
- `host_poll(NULL)` does nothing; the store/web code tolerates a missing host.
- Tests: `test_config` (config, hotkey, local address), degraded-mode and
  hotkey fields in `test_web`, credentials restore in `test_vpad`, the dead
  controller in `test_host`.

## 0.3.1-experimental (3 October 2026)

First build after the first run on a console.

- **Fixed: it stopped at start-up.** `scePadSetProcessPrivilege` was called
  before `scePadInit` and its failure was fatal. On the console it answers
  `0x80920005` (`SCE_PAD_ERROR_NOT_INITIALIZED`) when called first. Now
  `scePadInit` goes first and a privilege answer other than 0 is only a
  warning. Details in
  [PRUEBAS.md](PRUEBAS.md#registro-de-pruebas-en-consola).
- Renamed from "padhost" to **AnyPad PS5**. The data folder is now
  `/data/anypad` (log `anypad.log`, flags `pair` and `stop`). The old
  `/data/padhost` is not used any more and can be deleted by hand.
- The store file's magic is now `ANYPAD02`.
- Web page on port 8095 (no PIN, by choice), with checks that need none.
- ELFs are named with their version.
- New test `test_vpad`, which runs the console's start-up code against
  stand-ins for its libraries; it fails on the 0.3.0 order.

## 0.3.0-experimental (3 October 2026)

First build tried on a console (PS5 fat, firmware 10.01, launched from a USB
drive with Payload Manager). SHA-256 `ffd592fb7b9d01a984f6b9ed615f1bb058fa82223f76f3334badc408a785e237`
for the build made on 2 October; the one run was a later 0.3 build from the same
source. It stopped at start-up (see 0.3.1) without touching Bluetooth.

Before this: 0.1 and 0.2 were never built as ELFs.
