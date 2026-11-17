# Cambios desde el Entregable 2 (apuntes para el documento E3)

Base: el prototipo descrito en el E2. Los números `#n` son los de la Tabla 4.1 del E2.

## 1. Cambios del E2 que quedaron resueltos

| E2 | Antes (E2) | Ahora (E3) |
|----|-----------|------------|
| #2 | Sin consola ni web | `query.cpp` (consola) y `web.cpp` (HTTP). Consola si stdin es TTY o `DSM_CONSOLE=1`. Web en `port_base + 10*slot + 3` si `web.enabled`. |
| #5 | El directorio servía cualquier página | Una página debe estar asignada: si no, `DSM_ENOTALLOC` y la app termina con un mensaje. |
| #10 | `n_names = 0` en JOIN_ACK | JOIN_ACK lleva la tabla de nombres. |
| #11 | DEAD solo en la vista local | Cada nodo marca DEAD por sus heartbeats; el coordinador además difunde `NODE_DEAD`. Todos ejecutan la limpieza (ver §3). |
| #12 | Sin LEAVE | `dsm_shutdown` envía `LEAVE`; los demás marcan LEFT. |
| #17 | `mm_dsm` con banderas en memoria | Usa `dsm_alloc_named` y `dsm_barrier`. |
| #18 | Sin segunda app | Pendiente: se adaptará una app pública de GitHub. |
| Sec. 7.1 | Directorio sin timeouts | `Directory::sweep` abandona transiciones sin respuesta tras 3/4 de `request_timeout_ms` (el solicitante recibe `DSM_ETIMEOUT`). |
| Sec. 7.1 | `dsm_alloc` local, sin free | Asignador distribuido (`alloc.cpp`), `dsm_free`. |
| Sec. 7.1 | Sin locks ni barreras | `sync.cpp`. |

## 2. API (`dsm.h`)

Se conservan los nombres del E2 (`dsm_init`, `dsm_shutdown`, `dsm_node_id` que devuelve `int`, `dsm_num_nodes`, `dsm_home_base/size`, `dsm_space_size`, `dsm_alloc`, `dsm_strerror`). Se **agregan**:

`dsm_alloc_named`, `dsm_lookup`, `dsm_free`, `dsm_lock`, `dsm_unlock`, `dsm_barrier`, `dsm_get_stats`, `dsm_get_node_stats`, `dsm_get_global_stats`, `dsm_last_error` y `dsm_stats_t` (96 B).

Los códigos `DSM_E*` pasaron de `kadp_types.h` a `dsm.h` (los usan el protocolo y las apps). `dsm_alloc` ahora devuelve memoria en cero, contigua, que puede cruzar rangos de varios nodos.

## 3. Módulos nuevos y comportamiento

- **`control.cpp` (ControlPlane):** llamadas petición/respuesta (`seq`, `F_IS_REPLY`) sobre el canal de control, también hacia el propio nodo. Un llamador espera hasta la respuesta, el timeout o la caída del destino (con 400 ms de gracia para respuestas en vuelo). Los listeners no esperan: responden o dejan la petición pendiente.
- **`alloc.cpp`:** mapa de páginas libres y registros por home. Asignación en dos pasos: (1) bloque dentro de un rango, empezando por el propio; (2) si no cabe, cola libre de un rango + inicios de los siguientes, con deshacer si una pieza falla. Los nodos pares asignan desde el inicio de su rango y los impares desde el final, para dejar libres los extremos que usan las asignaciones que cruzan rangos. `dsm_free` invalida todas las copias y pone la página en ceros antes de confirmar.
- **`names.cpp`:** el coordinador decide quién registra primero; réplica en todos los nodos. Si dos nodos crean el mismo nombre a la vez, el perdedor libera su bloque y usa el del ganador.
- **`sync.cpp`:** locks con cola FIFO y barreras en el coordinador. Varios hilos del mismo nodo se serializan localmente (el coordinador conoce nodos, no hilos).
- **`query.cpp` / `web.cpp`:** comandos `help, nodes, stats [global|id], pages local|global, page <id>, allocs, names, quit` y su vista HTML (recarga cada 2 s).
- **`stats.h`:** contadores globales (fallos, páginas, invalidaciones, latencia de fallo) y bytes por `Network`.
- **Caída o salida de un nodo (cada nodo, una vez):** sus llamadas pendientes fallan con `DSM_ENODEDOWN`; sus páginas home pasan a LOST (copias locales descartadas, acceso aborta con mensaje); en cada directorio su copia sale de los copysets y las páginas que tenía en M pasan a LOST; el coordinador libera sus locks y falla las barreras en curso.
- **Pager:** si el home está caído el fallo se resuelve de inmediato con `DSM_ENODEDOWN`; el mensaje de `abort()` indica el motivo (ENODEDOWN, ENOTALLOC, ETIMEOUT).

## 4. Desviaciones respecto al diseño del E1

- `ALLOC_REQUEST` lleva además `alloc_start`, tamaño y nombre, y tiene modos ANY y TAIL (banderas `F_ALLOC_ANY`, `F_ALLOC_TAIL`). `FREE_ACK` trae el registro liberado; `FREE` con `n_pages = 0` significa "la asignación que empieza aquí".
- Mensajes nuevos: `NAME_REMOVE` (0x28), `ALLOCS_REQUEST` (0x44), `ALLOCS_REPLY` (0x45). `NAME_ANNOUNCE` lleva un byte de operación (alta/baja). `PAGE_TABLE_REQUEST` lleva un byte de modo y su respuesta incluye el estado y el copyset del directorio. `LOCK_RELEASE` tiene respuesta.
- Coordinador = menor ID que no esté DEAD ni LEFT (un SUSPECT conserva el rol, para no oscilar). `NODE_ANNOUNCE` sale por el ControlPlane.
- Un JOIN repetido de la misma IP y slot solo se trata como reintento dentro de `2 * request_timeout_ms`; pasado ese tiempo el anterior se declara DEAD y el nuevo recibe otro ID. Un nodo DEAD o LEFT no revive por heartbeats.
- `PageEntry.alloc_id/alloc_order` solo se llenan en el home de la página.

## 5. Limitaciones que siguen abiertas

- JOIN sin reenvío multi-salto (el nodo nuevo recorre hosts x slots hasta dar con el coordinador).
- Si cae el coordinador se pierden sus tablas de locks y barreras. Si cae cualquier nodo, las barreras en curso fallan.
- Las páginas cuyo home salió o murió, y las que tenía en M un nodo caído, se pierden (sin el extra de tolerancia a fallos).
- Una asignación que cruza rangos necesita libres la cola de un rango y el inicio del siguiente.
- Todo acceso pasa por la red, incluso el de la propia página home (sin atajo local).
- La IP anunciada es la primera IPv4 que no sea loopback ni virtual; no hay parámetro para forzarla.
- `selftest_dsm` e `idle_dsm` son herramientas de prueba propias; las aplicaciones del enunciado (`mm_dsm` y la segunda) se adaptan de GitHub.

## 6. Pruebas nuevas (`make itest`)

- `tests/test_cluster.sh [n]`: `selftest_dsm` en n nodos (escenarios 1, 2, 3 y 5 de la Tabla 4.3 del E1, locks, nombres, free, asignación que cruza rangos, ENOMEM, estadísticas) y acceso a página no asignada. Verificado con 2, 3 y 4 nodos.
- `tests/test_apps.sh [n] [N]`: `mm_dsm` (la segunda app se agrega al elegirla).
- `tests/test_failure.sh`: `kill -9` de un nodo que tiene un lock; se ve DEAD en unos 5 s, el lock pasa al que esperaba, y un `quit` se ve como LEFT.
