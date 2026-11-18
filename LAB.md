# Prueba en el laboratorio: guía paso a paso

Guía completa para correr KADP-DSM en el laboratorio (varias PC, Linux Mint 20.3). Sigue los
pasos **en este orden**, sin saltar ninguno la primera vez. Ejemplo con 3 PC (PC1, PC2, PC3),
un nodo por PC, nodos 0/1/2. Si usan otra cantidad de PC o de nodos por PC, el procedimiento es
el mismo; solo cambian los números (`max_nodes`, la lista de IPs, cuántos procesos lanzan por PC).

Datos conocidos del laboratorio (ver `documentacion/Proyecto_SO_DSM_Entorno_Laboratorio.md` en el
repo de documentación): Linux Mint 20.3 "Una" (Ubuntu 20.04 base), kernel 5.4, g++ 9.4, glibc 2.31,
hostname `soporte` en todas las máquinas (por eso se usan IPs, no hostnames), rango de puertos
asignado a la pareja **28100–28199**. La interfaz `172.17.0.1` (o `docker0` en `ip -br addr`) es el
puente de Docker: **nunca** usarla en `dsm.conf`.

---

## 0. Antes de ir al laboratorio (en casa, con tiempo)

1. Confirmar que el código compila con una cadena de herramientas parecida a la del laboratorio.
   Hay un contenedor `ubuntu:20.04` a mano para esto (no reemplaza probar en el laboratorio, pero
   atrapa errores de C++20 o de banderas de GCC moderno):
   ```bash
   cd DSM-System
   docker run --rm -it -v "$PWD":/src -w /src ubuntu:20.04 \
     bash -c "apt update && apt install -y build-essential && make clean all"
   ```
   Debe terminar sin errores. Si algo no compila ahí pero sí en Debian 13, es casi siempre C++20
   (`<span>`, `std::jthread`, conceptos) o una bandera que GCC 9 no reconoce.
2. Revisar que `make test` pase en limpio en la máquina de desarrollo:
   ```bash
   make clean && make && make test
   ```
   Las 7 pruebas unitarias (`test_config`, `test_log`, `test_membership`, `test_memory`,
   `test_network`, `test_pager`, `test_protocol`) deben imprimir `OK`.
3. Tener el repositorio accesible desde el laboratorio: lo más simple es poder hacer
   `git clone https://github.com/KA21Software/DSM-System` desde ahí. Si el laboratorio no tiene
   salida a internet, llevarlo en una USB (la carpeta completa, o un `git archive`) y copiarlo a
   cada PC por USB o por red local una vez allí.
4. **No llevar binarios compilados en Debian 13.** Quedan enlazados contra glibc 2.41 y en
   Linux Mint (glibc 2.31) fallan con algo como:
   ```
   ./build/apps/mm_dsm: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.3x' not found
   ```
   Se copia el **código fuente** y se compila en cada PC del laboratorio con `make`.
5. Imprimir o guardar esta guía (o tenerla abierta en el teléfono): en el laboratorio puede no
   haber red para abrir el repositorio de documentación.

---

## 1. Al llegar: verificar el entorno de UNA PC del laboratorio

Antes de copiar nada, correr esto en una sola PC para confirmar que coincide con lo esperado:

```bash
cat /etc/os-release        # esperado: Linux Mint 20.3 "Una"
uname -r; uname -m          # esperado: 5.4.0-xxx-generic, x86_64
ldd --version | head -1     # esperado: glibc 2.31
g++ --version               # esperado: g++ 9.4 o similar (debe soportar -std=c++17)
make --version | head -1
git --version
```

- Si `g++` es la 9.x, compilar normal con `make`.
- Si por algún motivo hay dos versiones de g++ instaladas y la default es muy vieja (<7, sin
  soporte completo de C++17), usar `make CXX=g++-9` (o la versión que exista) en vez de `make`
  en **todos** los pasos siguientes.
- Si falta `g++`, `make` o `git` y no hay `sudo` para instalarlos, avisar al profesor o soporte:
  sin esto no se puede continuar en esa PC.

```bash
hostname; hostname -I       # anotar la IP real de esta PC (ej. 172.19.13.75)
ip -br addr                 # identificar la interfaz del laboratorio vs docker0/virbr0
ss -tlnp | grep 281         # debe salir vacío: nadie debe estar ya usando 28100-28199
sudo ufw status             # si hay permisos; si no, lo descubriremos en el paso 4
```

La IP "buena" es la que empieza igual que las demás PC del laboratorio (ej. `172.19.13.x`).
Interfaces como `docker0` (típicamente `172.17.0.1`), `virbr0` o `lo` (`127.0.0.1`) **no sirven**
para hablar con otras PC: si `dsm.conf` termina usando esa IP, los demás nodos nunca conectan.

---

## 2. Clonar y compilar en CADA PC

Repetir esto en PC1, PC2 y PC3 (una terminal en cada una):

```bash
git clone https://github.com/KA21Software/DSM-System
cd DSM-System
make                 # o: make CXX=g++-9   si el default no sirve
```

**Qué esperar:** termina sin errores y, salvo algún warning esperado y ya conocido del equipo, sin
warnings nuevos. Se generan `build/libdsm.a`, `build/tests/test_*` y `build/apps/{mm_dsm,
selftest_dsm,idle_dsm}`.

**Errores posibles:**
- `g++: command not found` → instalar `build-essential` si hay `sudo`, o usar otra PC.
- Errores de sintaxis C++20 (`concepts`, `<span>` no existe en 9.4) → no debería pasar si el
  paso 0.1 se hizo bien; si pasa, es un cambio reciente del código que no se probó en Ubuntu 20.04.
- `Makefile:xx: *** missing separator` → el archivo se dañó al copiarlo (tabs vs espacios); volver
  a clonar/copiar en vez de editar a mano.

Luego, en cada PC:

```bash
make test            # pruebas unitarias
```

**Qué esperar:** 7 líneas, cada una empezando con `== build/tests/test_...` seguida de `OK`. Si
alguna prueba falla aquí (y no falló en Debian 13), es una diferencia de entorno real (kernel,
glibc, timing) y hay que investigarla **antes** de seguir con el resto de los pasos: ejecutar esa
prueba sola y leer el mensaje (`./build/tests/test_memory`, etc.).

---

## 3. Anotar las IPs reales de cada PC

En cada PC:

```bash
ip -4 addr               # o: hostname -I
```

Anotar la IP de la interfaz real del laboratorio (no `docker0`, no `127.0.0.1`), por ejemplo:

| PC  | IP anotada      |
|-----|-----------------|
| PC1 | 172.19.13.75    |
| PC2 | 172.19.13.XX    |
| PC3 | 172.19.13.YY    |

Si `nodes` (más adelante, paso 8.1) muestra para algún nodo una IP que no coincide con la tabla
(por ejemplo `172.17.0.1`), esa PC tiene varias interfaces y el DSM elige la primera IPv4 que no
es loopback, que puede no ser la correcta: avisar, porque **no hay** parámetro en `dsm.conf` para
forzar la IP de salida; la solución es deshabilitar temporalmente la interfaz que sobra
(`sudo ip link set docker0 down`, si hay `sudo`) o aislar la PC de esa red.

---

## 4. Prueba de conectividad entre las PC (antes de tocar el DSM)

El puerto de **control** del slot 0 es `28101` (`port_base=28100 + port_stride=10 * slot=0 +
offset=1`). Usamos ese para probar porque es el que usa el JOIN.

En **PC2**, dejar algo escuchando:
```bash
nc -l 28101
```
En **PC1**, probar la conexión:
```bash
nc -vz <ip_pc2> 28101
```
Debe imprimir algo con `succeeded` (ej. `Connection to 172.19.13.XX 28101 port [tcp/*] succeeded!`).
Repetir en ambos sentidos y entre todas las parejas de PC (PC1↔PC2, PC1↔PC3, PC2↔PC3). Con 3 PC
son 6 pruebas (o 3 si confían en que el firewall trata igual ambos sentidos).

**Si falla** (`nc: connect to <ip> port 28101 (tcp) failed: Connection refused` o se queda
colgado sin conectar):
```bash
sudo ufw status                       # ¿está activo? ¿bloquea 28100-28199?
sudo ufw allow 28100:28199/tcp         # si hay permisos de sudo y el profesor lo autoriza
ip -4 addr                             # ¿PC1 y PC2 están en la misma subred?
ping -c2 <ip_pc2>                      # ¿hay ruta siquiera a nivel IP?
```
Si ni el `ping` contesta, el problema es de red (switch, VLAN, cable), no del firewall ni del
DSM: hay que resolverlo con el encargado del laboratorio antes de seguir. **No tiene sentido**
pasar al paso 5 si esta prueba no funciona entre todas las PC: el DSM fallará exactamente igual
y será más difícil diagnosticar por qué.

---

## 5. Configuración (`conf/lab.conf`), idéntica en todas las PC

En **una** PC, copiar y editar:
```bash
cp conf/dsm.conf conf/lab.conf
```
Editar `conf/lab.conf` con los valores reales (ejemplo con 3 PC y 48 MB de espacio compartido
total: 3 nodos × 16 MB):
```ini
[cluster]
name = kadp-dsm
hosts = 172.19.13.75,172.19.13.XX,172.19.13.YY   # IPs reales, en orden, separadas por coma
max_nodes = 3                                     # cantidad de nodos de ESTA corrida
max_nodes_per_host = 4                            # tope de nodos por PC (no cambia si no hace falta)

[memory]
page_size = 4096
node_memory = 16M                                 # por nodo; 3 x 16M = 48 MB de espacio DSM total
base_address = 0x600000000000

[network]
port_base = 28100                                 # NO cambiar: es el rango asignado a la pareja
port_stride = 10
connect_timeout_ms = 2000
request_timeout_ms = 3000
max_retries = 3
connect_retry_ms = 500

[heartbeat]
interval_ms = 1000
suspect_after = 3
dead_after = 5

[cache]
max_cached_pages = 0

[log]
dir = ./logs
level = INFO                                      # usar DEBUG solo si hay que ver cada transición

[web]
enabled = true

[extras]
replication_factor = 0
```

**Qué debe coincidir exactamente entre las 3 PC** (el nodo lo valida al unirse: calcula un hash
de estos 4 campos y rechaza con `DSM_ECONFIG` si no coincide):
- `page_size`
- `node_memory`
- `max_nodes`
- `base_address`

El resto de los campos (timeouts, heartbeat, `log.level`, puertos, `hosts`) **no** se valida entre
nodos, pero deben ser razonablemente iguales para que el comportamiento sea el esperado (en
particular, `hosts` debe listar las mismas IPs en las 3 PC, para que cada nodo pueda encontrar a
los demás).

Copiar este mismo archivo a las otras 2 PC. La forma más confiable es `scp` desde PC1:
```bash
scp conf/lab.conf usuario@172.19.13.XX:~/DSM-System/conf/lab.conf
scp conf/lab.conf usuario@172.19.13.YY:~/DSM-System/conf/lab.conf
```
Si no hay SSH habilitado entre las PC del laboratorio, copiarlo por USB o escribirlo a mano en
cada PC con mucho cuidado de que quede **carácter por carácter igual** en `hosts`, `max_nodes`,
`node_memory`, `page_size` y `base_address`.

**Verificación rápida antes de seguir:** en cada PC,
```bash
cat conf/lab.conf | md5sum
```
Las 3 salidas deben ser idénticas (o al menos iguales en los 4 campos listados arriba).

---

## 6. Orden de arranque (importante, siempre igual)

**Reglas, en este orden:**
1. El nodo que va en el slot 0 (quien primero se identifica y arranca es, en la práctica, el
   que forma el cluster) se lanza **primero**, y se espera **~2 segundos** antes de lanzar los
   demás.
2. Cada PC lanza su(s) nodo(s) **en terminales separadas**, una por nodo, para poder ver su salida
   y usar su consola de forma independiente.
3. Cada aplicación espera internamente a que los `max_nodes` configurados estén presentes antes
   de empezar a trabajar (verán el mensaje `[node N] up` o el prompt de la consola mientras
   esperan).
4. Si hay que reiniciar una corrida completa, **cerrar todos los procesos de todas las PC
   primero** (`quit` en cada consola, o `Ctrl+C`/`kill`) antes de volver a arrancar desde el
   nodo 0. Un reinicio parcial consume IDs de nodo nuevos cada vez (los IDs no se reutilizan) y
   puede agotar el límite de `max_nodes` si se reinicia muchas veces sin cerrar todo.

---

## 7. Encender la consola interactiva y la vista web

Cada aplicación (`idle_dsm`, `selftest_dsm`, `mm_dsm`) activa su consola de comandos
automáticamente si se lanza desde una terminal normal (si `stdin` es un TTY). Si se lanza desde un
script o con la entrada redirigida, forzarla con la variable de entorno:
```bash
DSM_CONSOLE=1 build/apps/idle_dsm conf/lab.conf 600
```
Con `[web] enabled = true` en `lab.conf` (ya está en el ejemplo del paso 5), cada nodo además
sirve las mismas consultas por HTTP en `http://<ip_de_su_pc>:<28100 + 10*slot + 3>/`. Para el
slot 0 (el único que usará cada PC en este ejemplo) es el puerto **28103**.

---

## 8. Baterías de prueba, en este orden

Todas las pruebas de esta sección siguen el orden del paso 6: nodo del slot 0 primero, esperar
~2 s, luego los demás. "En cada PC" significa una terminal por PC, lanzando el comando indicado
con **el mismo** `conf/lab.conf`.

### 8.1 Cluster vacío: consola, tabla de nodos, vista web

En PC1 (primero):
```bash
build/apps/idle_dsm conf/lab.conf 600
```
Esperar ~2 s. Luego, en PC2 y PC3:
```bash
build/apps/idle_dsm conf/lab.conf 600
```
**Qué esperar:** cada terminal imprime `[node N] up (plain)` con su propio ID (0, 1 y 2, en el
orden en que arrancaron). Las tres quedan con un prompt de consola.

En cualquiera de las tres terminales, probar los comandos (ver la tabla completa en la sección 9):
```
nodes
stats
stats global
pages local
allocs
names
```
- `nodes` debe mostrar **3 filas**, todas en estado `ALIVE`, con la IP:slot correcta de cada PC
  (comparar contra la tabla del paso 3) y una marcada `(coordinador)` — debe ser el nodo con el
  ID numéricamente más bajo que esté vivo (normalmente el 0, el que arrancó primero).
- Si alguna fila muestra una IP que no es la esperada (docker0, virbr0, 127.0.0.1...), es el
  problema descrito en el paso 3: avisar y corregir antes de seguir.
- `allocs` y `names` deben salir vacíos (`(0 asignaciones)`, `(0 nombres)`): todavía no se ha
  asignado memoria con nombre.

Abrir en un navegador (de cualquier PC con acceso a la red del laboratorio) `http://<ip_pc1>:28103/`
y comprobar que se ve la misma tabla de nodos, con enlaces a `/stats`, `/stats/global`, `/pages`,
`/pages/global`, `/allocs`, `/names` (se recarga solo cada 2 s).

No cerrar estas 3 terminales todavía si se quiere encadenar la prueba 8.5 más abajo; si no, cerrar
cada una con `quit` (ver efecto `LEFT` más abajo, en 8.5) o `Ctrl+C`.

### 8.2 Autoprueba (`selftest_dsm`): concurrencia, locks, nombres, free, asignación grande

En PC1:
```bash
build/apps/selftest_dsm conf/lab.conf 3
```
Esperar ~2 s, luego en PC2 y PC3 el mismo comando.

**Qué prueba** (en orden, cada uno imprime una línea `[node N] <nombre>: done` o falla con
`FAIL`): escrituras concurrentes de todos los nodos sobre una misma página (`t1`), una página que
sube de S a M (`t_upgrade`), varios hilos del mismo nodo accediendo a la vez (`t_threads`), un lock
(`dsm_lock`/`dsm_unlock`), acceso cruzado entre los rangos (home) de distintos nodos
(`t_cross_homes`), nombres (`dsm_alloc_named`/`dsm_lookup`), liberación (`dsm_free`), una
asignación grande que puede cruzar el límite entre rangos de nodos (`t_big`), y por último que las
estadísticas (`dsm_get_stats`) tengan sentido (`t_stats`).

**Qué esperar al final:** cada nodo imprime una sola línea
```
[node N] SELFTEST PASS
```
y el proceso termina con código de salida 0. Si algo falla, se ve antes una o más líneas
`[node N] FAIL line <L>: <detalle>` y al final `SELFTEST FAIL` (código de salida 1); copiar esas
líneas tal cual para el informe, junto con los logs de ese nodo (ver sección 11).

### 8.3 Prueba adicional de `selftest_dsm`: página no asignada → `DSM_ENOTALLOC`

Opcional pero recomendada una vez, para ver el error documentado: en PC1 (y, si se quiere ver en
las 3 a la vez, también en PC2 y PC3, siguiendo el mismo orden):
```bash
build/apps/selftest_dsm conf/lab.conf 3 notalloc
```
**Qué esperar:** el proceso imprime `[node N] touching an unallocated page...` y termina
abruptamente (NO imprime `FAIL: read ... from an unallocated page`, que sería el caso de error del
propio sistema). El mensaje de `abort()` debe mencionar `ENOTALLOC`. Esto confirma que el
directorio rechaza servir páginas que nadie asignó.

### 8.4 Matrices (`mm_dsm`)

En PC1:
```bash
build/apps/mm_dsm conf/lab.conf 600 600 600 0 3
```
(`600 600 600` = N K M de las matrices A[N×K], B[K×M], C[N×M]; `0` = no imprimir las matrices;
`3` = cantidad de nodos.) Esperar ~2 s, y en PC2 y PC3 exactamente el mismo comando.

**Memoria necesaria:** A, B y C se reparten entre los 3 nodos; con N=K=M=600 y `double` (8 bytes),
cada matriz ocupa 600×600×8 ≈ 2.7 MB, las tres ≈ 8.2 MB, repartidas entre 3 nodos de 16 MB cada
uno (48 MB totales en `lab.conf`): sobra margen. Si `node_memory` del paso 5 fuera menor, subirlo
ahí (igual en las 3 PC) antes de probar tamaños más grandes.

**Qué esperar:** cada nodo imprime su progreso y tiempo; el **nodo 0** (y solo él) imprime al
final algo con `verification` y `OK` (verifica 200 entradas de C contra el cálculo esperado). Si
dice algo distinto de OK, hay un error de coherencia real: guardar la salida completa y los logs.

Para estresar un poco más, repetir con un tamaño mayor (por ejemplo `1000 1000 1000`), siempre que
`node_memory × 3` sea suficiente para A+B+C (1000×1000×8×3 ≈ 24 MB: con 16 MB por nodo, 48 MB
totales, también cabe).

### 8.5 Segunda aplicación

*(Pendiente: se agrega a esta guía cuando el equipo elija la segunda app de GitHub, según
`CAMBIOS_E3.md` §5. Usar el mismo patrón: nodo 0 primero, un proceso por PC, mismo `lab.conf`.)*

### 8.6 Caída de un nodo (membresía y recuperación del lock)

Repite el escenario de `tests/test_failure.sh` pero entre PC reales.

En PC1:
```bash
build/apps/idle_dsm conf/lab.conf 600
```
Esperar ~2 s. En PC2 (va a sostener un lock):
```bash
build/apps/idle_dsm conf/lab.conf 600 holder
```
En PC3 (va a esperar ese lock):
```bash
build/apps/idle_dsm conf/lab.conf 600 waiter
```
**Qué esperar antes de matar nada:**
- PC2 imprime `[node 1] holder: lock 5 -> OK` (toma el lock 5 y lo retiene).
- PC3, tras ~2 s, intenta el mismo lock y queda esperando (el lock 5 está ocupado por el nodo 1).
- En la consola de PC1, `nodes` muestra los 3 nodos `ALIVE`.

Ahora simular la caída de PC2 — cualquiera de las dos formas:
- **Desconectar el cable de red** de PC2 (o apagar su interfaz: `sudo ip link set <iface> down`).
- O, más simple para repetir varias veces, matar el proceso en PC2: en otra terminal de PC2,
  ```bash
  pkill -9 -f idle_dsm
  ```
  (o `kill -9 <pid>` del proceso que se vio con `ps aux | grep idle_dsm`).

**Qué esperar después, con los tiempos del `lab.conf` del paso 5** (`heartbeat.interval_ms=1000`,
`suspect_after=3`, `dead_after=5`: el nodo pasa a SUSPECT a los ~3 s sin heartbeat y a DEAD a los
~5 s):
- En la consola de PC1, escribir `nodes` unos 5–6 segundos después de la caída: el nodo 1 (PC2)
  debe aparecer como **DEAD** (puede verse primero como `SUSPECT` si se consulta justo a los 3-4
  s).
- En la terminal de PC3, debe aparecer `[node 2] waiter: GOT LOCK`: el lock que tenía el nodo
  muerto se liberó y pasó a quien esperaba.
- Si en vez de eso PC3 se queda esperando indefinidamente (nunca imprime `GOT LOCK`), es un
  problema real de limpieza de locks al caer un nodo: guardar logs de los 3 nodos.

Para ver el otro caso de membresía (salida ordenada, no una caída), en la consola de PC3 (o la que
quede) escribir:
```
quit
```
y en PC1, `nodes` debe mostrar ese nodo como **LEFT** (distinto de `DEAD`: salió avisando, no cayó).

### 8.7 Estrés de memoria (`DSM_ENOMEM`)

Ya cubierto indirectamente por `selftest_dsm` (paso 8.2, escenario `t_big`), pero para verlo de
forma explícita con `mm_dsm`: subir `N K M` hasta que la memoria repartida entre los 3 nodos no
alcance para A+B+C. Por ejemplo, con `node_memory=16M` (48 MB totales) y matrices de `double`,
el límite aproximado es cuando `3 × N × N × 8 bytes` se acerca a 48 MB, es decir N cerca de 1400.
Probar con un N claramente por encima (p. ej. 2000):
```bash
build/apps/mm_dsm conf/lab.conf 2000 2000 2000 0 3
```
**Qué esperar:** el programa debe terminar con un mensaje de error relacionado con
`DSM_ENOMEM` (no debe colgarse ni dar una violación de segmento sin mensaje). Si lo que pasa es un
cuelgue sin mensaje, es un problema a reportar, no el comportamiento esperado.

---

## 9. Comandos de la consola interactiva (referencia completa)

Disponibles en cualquier nodo cuyo `stdin` sea una terminal (o con `DSM_CONSOLE=1`). Escribir el
comando y Enter en la terminal donde corre ese nodo:

| Comando | Qué muestra |
|---|---|
| `help` | esta misma lista |
| `nodes` | tabla: ID, dirección `ip:slot`, estado (`ALIVE`/`SUSPECT`/`DEAD`/`LEFT`), memoria aportada, memoria libre, segundos desde el último heartbeat; marca `(coordinador)` y `(este nodo)` |
| `stats` | estadísticas de **este** nodo: nodos vivos, páginas home usadas/totales, páginas en caché (S y M), fallos de lectura/escritura, páginas enviadas/recibidas, invalidaciones enviadas/recibidas, bytes enviados/recibidos, latencia de fallo promedio y máxima |
| `stats global` | lo mismo, pero sumado entre todos los nodos vivos |
| `stats <id>` | lo mismo, pero de un nodo remoto específico (consulta por red; si ese nodo no responde, da el error correspondiente) |
| `pages local` | páginas que tiene **este** nodo en memoria: número, estado MSI (`I`/`S`/`M`/`IS_P`/`IM_P`/`SM_P`), versión, id de asignación, orden, home, y si está `LOST` |
| `pages global` | recorre **todos** los homes: página, home, estado del directorio (`SHARED`/`MODIFIED`), dueño, copyset (qué nodos tienen copia), versión, id de asignación; nodos `DEAD`/`LEFT` se marcan como "sus páginas se perdieron" |
| `page <id>` | el estado de una página concreta en todos los nodos (vista detallada de una sola fila) |
| `allocs` | todas las asignaciones activas: id, nombre (si lo tiene), nodo y PID dueño, páginas que ocupa, tamaño en bytes |
| `names` | tabla de asignaciones **con nombre** (`dsm_alloc_named`): nombre, id de asignación, primera página, tamaño, nodo creador |
| `quit` / `exit` | este nodo sale ordenadamente (los demás lo verán como `LEFT`, no `DEAD`) |

Ejemplo de uso típico durante cualquier prueba: dejar `nodes` y `stats global` como primera
comprobación siempre, y usar `pages global` o `page <id>` solo cuando se sospecha de un problema
de coherencia concreto.

---

## 10. Vista web (si no quieren usar la terminal, o para mostrarlo en el demo)

Con `[web] enabled = true`, cada nodo en el slot *s* de una PC sirve en
`http://<ip_de_esa_pc>:<28100 + 10*s + 3>/` (slot 0 → puerto 28103). Rutas disponibles, todas de
solo lectura y que se recargan cada 2 s:

| Ruta | Equivalente en consola |
|---|---|
| `/` | `nodes` |
| `/stats` | `stats` |
| `/stats/global` | `stats global` |
| `/pages` | `pages local` |
| `/pages/global` | `pages global` |
| `/allocs` | `allocs` |
| `/names` | `names` |

Útil para proyectarlo en el demo final sin saturar una terminal con `nodes` repetido a mano, y
para revisar el estado de un nodo desde otra PC sin necesidad de tener su consola abierta.

---

## 11. Logs: dónde están y qué buscar

Cada proceso escribe su propio log en el directorio configurado en `[log] dir` (en el `lab.conf`
del paso 5, `./logs`, relativo a donde se ejecutó el comando), con un nombre tipo
`node-<pid>.log`.

```bash
ls logs/                          # un archivo por proceso lanzado en esa PC
tail -f logs/node-<pid>.log       # seguir un log en vivo mientras se prueba
grep -E "WARN|ERROR|DEAD|SUSPECT|LOST" logs/*.log     # buscar problemas rápidamente
```

Con `level = DEBUG` en vez de `INFO` (cambiar en `lab.conf` **en las 3 PC** y relanzar) se ve cada
`PAGE_REQUEST`, `INVALIDATE`, `FETCH`, etc. — útil para depurar un caso puntual de coherencia, pero
genera mucho texto: no dejarlo así para toda la sesión de pruebas, solo para repetir el caso que
falló.

**Guardar siempre** (para el informe, y para poder repetir el análisis sin volver al laboratorio):
copiar la carpeta `logs/` completa de cada PC apenas termine cada prueba de la sección 8, antes de
borrarla o de que la sobreescriba la siguiente corrida (por ejemplo, `cp -r logs logs_8.2_pc1`).

---

## 12. Otras formas de monitorear mientras corre una prueba

| Qué ver | Cómo |
|---|---|
| Conexiones TCP del DSM abiertas en esta PC | `ss -tn | grep ':281'` |
| Tráfico real entre nodos (requiere `sudo`) | `sudo tcpdump -i <iface> tcp portrange 28100-28199 -c 50` |
| Procesos del DSM corriendo en esta PC | `ps aux | grep -E "mm_dsm|selftest|idle_dsm"` |
| Uso de memoria/CPU de un proceso puntual | `top -p <pid>` o `cat /proc/<pid>/status` |

---

## 13. Códigos de error (`DSM_E*`) y qué significan

Toda función de la API que puede fallar devuelve uno de estos códigos (o lo entrega
`dsm_strerror()` / `dsm_last_error()`); también aparecen en los mensajes de `abort()` cuando un
fallo de página no se puede resolver:

| Código | Significado | Cuándo aparece en estas pruebas |
|---|---|---|
| `DSM_EINVAL` | argumento inválido (ej. tamaño 0) | uso incorrecto de la API, no debería verse en las apps de prueba tal como están |
| `DSM_ENOMEM` | no hay memoria libre suficiente en el DSM | paso 8.7, o `selftest_dsm` (`t_big`) |
| `DSM_EEXIST` | un nombre ya existe con otro tamaño | dos `dsm_alloc_named` del mismo nombre con tamaños distintos |
| `DSM_ENOTFOUND` | `dsm_lookup` de un nombre que no existe | normal si se consulta antes de que alguien lo cree |
| `DSM_ENODEDOWN` | el nodo dueño de la página (u otro nodo necesario) cayó o salió | paso 8.6, al tocar una página cuyo home murió |
| `DSM_ETIMEOUT` | el home de una página no respondió en `request_timeout_ms` | problema de red o de carga; revisar logs de ambos nodos |
| `DSM_ENET` | error de red (socket, conexión) | caída abrupta de la red entre dos PC |
| `DSM_ECONFIG` | configuración distinta entre nodos, o archivo mal formado | paso 5 mal copiado entre PC; revisar `page_size`, `node_memory`, `max_nodes`, `base_address` |
| `DSM_ENOTINIT` | se llamó a la API antes de `dsm_init` o después de `dsm_shutdown` | no debería verse en las apps tal como están escritas |
| `DSM_EFULL` | el cluster ya tiene `max_nodes` nodos y llega uno más | se lanzó un proceso de más, o `max_nodes` quedó bajo |
| `DSM_EPROTO` | mensaje de red mal formado o inesperado | indicaría una incompatibilidad de versión de binarios entre PC |
| `DSM_ENOTALLOC` | se accedió a una página que nadie asignó | paso 8.3, a propósito |

---

## 14. Problemas comunes y qué hacer

- **`dsm_init` falla de entrada / mensaje con `DSM_ECONFIG`:** el `lab.conf` no es idéntico en
  `page_size`, `node_memory`, `max_nodes` o `base_address` entre las PC (sección 5), o el archivo
  tiene una línea mal escrita (sección mal cerrada, falta `=`). Releer el archivo completo en la
  PC que falla.
- **El nodo 0 no ve a los demás / la consola nunca aparece, queda "esperando":** puertos
  bloqueados o alguna IP de `hosts` está mal escrita; repetir la prueba de conectividad del
  paso 4 exactamente con las IPs que están en `lab.conf`.
- **Se aborta con `DSM_ETIMEOUT`:** el home de esa página no respondió dentro de
  `request_timeout_ms` (3 s en el ejemplo); puede ser red lenta/saturada o que ese nodo esté
  bloqueado en algo — revisar `logs/` de los dos nodos implicados (el que pidió y el home).
- **Se aborta con `DSM_ENODEDOWN`:** el nodo dueño de esa página cayó o salió; es el
  comportamiento esperado (ver sección 13 y paso 8.6), no un bug — las páginas de un nodo caído se
  pierden.
- **`DSM_ENOMEM` al asignar memoria:** subir `node_memory` (igual en todas las PC) o bajar el
  tamaño del problema (`N K M` más chicos en `mm_dsm`).
- **Un nodo aparece con una IP rara (`172.17.0.1`, `10.x`, etc.) en `nodes`:** ver paso 3; esa PC
  tiene una interfaz extra (Docker, VPN, red virtual) y el DSM tomó la IP equivocada.
- **`scp` o SSH no funciona entre las PC del laboratorio:** copiar `lab.conf` por USB, o escribirlo
  a mano con mucho cuidado en cada PC (comparar con `md5sum`, paso 5).
- **Al reiniciar una prueba, el primer nodo tarda o se comporta raro:** seguramente quedó algún
  proceso anterior vivo; comprobar con `ps aux | grep -E "mm_dsm|selftest|idle_dsm"` en cada PC y
  matarlos (`kill`, o `kill -9` si no responde) antes de relanzar desde el nodo 0.
- **Warnings de compilación nuevos que no salían en Debian 13:** son casi siempre código que GCC 9
  interpreta distinto a GCC 14; no ignorarlos, revisar la línea señalada.

---

## 15. Al terminar: qué guardar para el informe

- La carpeta `logs/` de **cada PC**, de cada prueba de la sección 8 (copiarla aparte antes de la
  siguiente corrida, como se indica en la sección 11).
- La salida de consola de cada terminal (copiar y pegar, o capturar con `script` /
  `tee salida.txt` si se quiere automatizar: `build/apps/mm_dsm conf/lab.conf 600 600 600 0 3 2>&1 | tee salida_pc1.txt`).
- Una captura de pantalla de la vista web (`/`, `/stats/global`) durante alguna prueba, para el
  informe o la presentación.
- Las IPs reales anotadas en el paso 3 y la versión de g++/kernel confirmada en el paso 1, por si
  hay que explicar alguna diferencia de comportamiento frente a las pruebas hechas en casa.
