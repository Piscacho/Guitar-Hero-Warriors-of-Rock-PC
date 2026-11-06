Guitar Hero: Warriors of Rock para PC
=====================================

No incluye archivos del juego. Necesitas tu propio ISO de Xbox 360 de Guitar Hero: Warriors of Rock
(version USA/Europa: "Guitar Hero - Warriors of Rock (USA, Europe) (En,Fr,De,Es,It)").

Primeros pasos
1. Abre "Guitar Hero Warriors of Rock.exe" (el launcher). Si Windows muestra "Windows protegio tu PC",
   pulsa Mas informacion -> Ejecutar de todas formas.
2. En la pestana Juego elige el ISO con "Examinar..." y, si tienes DLC, su carpeta.
3. Ajusta imagen, controles y microfono si quieres, y pulsa JUGAR.

- Para entrar directo al juego desactiva "Mostrar este launcher al iniciar" (manten Mayus al abrir el
  .exe para volver a verlo). "Guitar Hero Warriors of Rock.exe --jugar" lo salta siempre (para accesos
  directos o Steam).

Controles
- Mando de Xbox o PlayStation: hace de guitarra, bateria o mando normal (voz). Guitarras y baterias de
  Xbox 360 para PC se detectan solas.
- Hasta 4 jugadores: cada mando es un jugador; en Controles eliges el instrumento de cada uno y puedes
  reasignar botones (para todos o solo para un jugador) pulsando "Asignar..." y el boton del mando.
- Los jugadores 2 a 4 entran como invitados: en el aviso de perfil elige "Continue without saving".
- F6 cambia el instrumento del jugador del teclado. F11 o Alt+Enter: pantalla completa. F12: captura.
- Preajustes del mando de Xbox en Controles -> Preajustes: clasico (A/B/Y/X/LB) o gatillos (LT/LB/RB/RT/A).
- Guitarra CRKD (Gibson Les Paul): Controles -> Preajustes -> "Guitarra CRKD" (palanca en LT, inclinacion en RT).
- Guitarra de otra marca que no responde bien: Controles -> "Informe de mandos" genera mandos.txt con lo que
  envia; con ese archivo se puede configurar.
- Teclado: flechas, Enter = Start, Retroceso = poder estrella, 1-5 = trastes (Espacio = verde,
  Esc = rojo), Q/E = gatillos. En bateria: 5 = platillo naranja, K o B = bombo.

Microfono
Cualquier microfono de Windows hace de microfono USB de la consola (pestana Microfono: dispositivo,
ganancia y medidor de nivel). Se asigna al primer jugador con instrumento Voz.

Juego en linea (experimental)
Partidas en linea con otros jugadores de esta version para PC, sin servidores: la conexion va directa entre
vuestros PCs. En el launcher, pestana "En linea": activalo, pon tu nombre y pasa tu codigo de sala a tu amigo
(pon el suyo en "Codigos de tus amigos"). En la misma red o en una LAN virtual (Radmin VPN, ZeroTier, Tailscale)
no hace falta codigo. En el juego: Quickplay+ -> FIND Xbox LIVE PLAYERS (boton naranja); el juego os junta solo.
Solo se pueden tocar las canciones y DLC que tengais todos. Si tu proveedor usa CGNAT (el launcher lo avisa),
poned los dos el codigo del otro o usad una LAN virtual. Windows puede pedir permiso al cortafuegos la primera vez.

DLC
Copia tus paquetes de contenido descargable de Xbox 360 (archivos LIVE/PIRS/CON, por ejemplo
41560883\00000002\<archivo>) en la carpeta dlc (puede tener subcarpetas). Se importan solos al arrancar.

Imagen
En "Imagen y audio": suavizado de bordes (FXAA), filtrado de texturas (anisotropico 16x) y nitidez (CAS).
Contador de FPS: F3 en el juego (o "Contador de FPS" en el launcher): FPS, tiempo por cuadro, retraso del audio y tirones.

Sincronia
Calibra una vez en el juego: Options -> Calibrate Lag -> Calibrate. En el launcher, "Latencia del audio"
baja el retraso del sonido (Baja / Normal / Segura).

Donde se guarda todo
- Partidas, DLC importado y respaldos: carpeta save junto a este archivo (conservala si actualizas a mano).
- Empezar de cero: Juego -> Partidas -> "Empezar de cero". La partida anterior queda en save\respaldos
  y se recupera con "Restaurar respaldo".
- Configuracion: gh6.ini. Registro de la ultima partida: gh6.log.

Actualizar
El launcher busca versiones nuevas al abrirse (Juego -> Actualizaciones) y se actualiza solo, conservando
gh6.ini, las partidas y el DLC.

Requisitos
- Windows 10 (1803 o posterior) / 11 de 64 bits y una tarjeta grafica con Direct3D 12.

Guitar Hero es una marca de Activision. Este proyecto no esta afiliado a Activision.
Licencias: ver LICENSE, THIRD-PARTY-NOTICES.txt y la carpeta licenses.
