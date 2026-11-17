# Prueba en el laboratorio: pasos y monitoreo

Ejemplo con 3 PC (PC1, PC2, PC3), un nodo por PC. Cambia los números si usan otra cantidad.

## 0. Antes de ir (en casa)
- Compilar con g++ 9 si puedes (el laboratorio tiene Mint 20.3): `make CXX=g++-9` o confirma con `g++ --version` en una PC del lab. Esto es lo primero que hay que verificar allá.
- Tener el repositorio en una USB o clonable: `git clone https://github.com/KA21Software/DSM-System`.

## 1. En cada PC (una vez)
```
cd DSM-System && make          # debe terminar sin errores ni warnings
make test                      # 7 pruebas deben imprimir OK
ip -4 addr                     # anota la IP de la interfaz real (ej. 10.0.0.11)
```

## 2. Conectividad
Desde PC1, para cada otra PC (el puerto de control del slot 0 es 28101):
```
ping -c2 <ip_pc2>
```
En PC2: `nc -l 28101` y en PC1: `nc -vz <ip_pc2> 28101` (debe decir "succeeded"). Repetir entre todas las parejas.
Si falla: `sudo ufw status` (abrir `28100:28199/tcp`) y revisar que estén en la misma subred.

## 3. Configuración (idéntica en todas las PC)
Copiar `conf/dsm.conf` a `conf/lab.conf` y editar:
```
[cluster]
hosts = 10.0.0.11,10.0.0.12,10.0.0.13     # las IP reales, separadas por coma
max_nodes = 3                              # cantidad de nodos de esta corrida
max_nodes_per_host = 4
[memory]
node_memory = 16M                          # 3 x 16 MB = 48 MB de espacio compartido
[log]
dir = ./logs
level = INFO                               # DEBUG para ver cada transición de página
```
Deben coincidir `page_size`, `node_memory`, `max_nodes` y `base_address` (si no, el JOIN se rechaza con `DSM_ECONFIG`). `scp conf/lab.conf` a las demás PC o editarlo igual en cada una.

## 4. Orden de arranque
**Siempre el nodo 0 primero, esperar ~2 s, luego los demás** (cada PC lanza su nodo en el slot 0). Cada app espera a que estén todos antes de empezar.

## 5. Pruebas (en este orden)

### 5.1 Cluster vacío y consola
En cada PC: `build/apps/idle_dsm conf/lab.conf 600` (PC1 primero). En cualquiera:
```
nodes          # los 3 ALIVE, uno marcado (coordinador), IP:slot correctos
stats global
```
Si un nodo muestra una IP que no es la del lab (docker, virbr0...), avísame: hay que forzar la IP.

### 5.2 Autoprueba (concurrencia, locks, nombres, free, asignación grande)
En cada PC: `build/apps/selftest_dsm conf/lab.conf 3`. Cada nodo debe terminar con `SELFTEST PASS`.

### 5.3 Matrices
En cada PC: `build/apps/mm_dsm conf/lab.conf 600 600 600 0 3`. El nodo 0 imprime `verification ... OK`. Probar también 1000 si `node_memory` alcanza (A+B+C = 3 x 8 MB).

### 5.4 Segunda aplicación
(pendiente: se agrega cuando se elija la app de GitHub)

### 5.5 Caída de un nodo
Con `idle_dsm` en las 3 PC (PC2 con rol `holder`: `build/apps/idle_dsm conf/lab.conf 600 holder`, PC3 con `waiter`): desconectar el cable o `kill -9` el proceso en PC2.
- En ~5 s `nodes` en PC1 muestra el nodo 1 como **DEAD**.
- PC3 imprime `GOT LOCK` (el lock del nodo muerto pasó al que esperaba).
- `quit` en la consola de un nodo se ve como **LEFT** en los demás.

### 5.6 Estrés
`dsm_alloc` por encima de la memoria total debe dar `DSM_ENOMEM` (lo cubre `selftest_dsm`). Para ocupar toda la memoria: matrices con `N` grande hasta que la app avise que no caben.

## 6. Cómo monitorear

| Qué | Cómo |
|-----|------|
| Estado de nodos, quién coordina, último heartbeat | consola: `nodes` |
| Fallos, páginas enviadas, bytes, latencia de fallo | `stats`, `stats <id>`, `stats global` |
| Dónde está cada página (dueño, copias) | `pages global`, `page <id>`, `pages local` |
| Asignaciones y nombres | `allocs`, `names` |
| Lo mismo en el navegador (recarga cada 2 s) | `http://<ip>:28103/` (slot 0). Rutas: `/`, `/stats`, `/stats/global`, `/pages`, `/pages/global`, `/allocs`, `/names` |
| Log detallado por proceso | `tail -f logs/node-pid*.log` (con `level = DEBUG` ves cada PAGE_REQUEST, INVALIDATE, FETCH) |
| Buscar problemas | `grep -E "WARN|ERROR|DEAD|SUSPECT|LOST" logs/*.log` |
| Conexiones TCP del DSM | `ss -tn | grep ':281'` |
| Tráfico real | `sudo tcpdump -i <iface> tcp portrange 28100-28199 -c 50` |
| Procesos | `ps aux | grep -E "mm_dsm|selftest|idle_dsm"` |

La consola está activa si el proceso se lanzó en una terminal (o con `DSM_CONSOLE=1`). Se escribe en la terminal donde corre el nodo; si la app imprime mucho, usa el navegador.

## 7. Qué hacer si algo falla
- **`dsm_init: ...` / `DSM_ECONFIG`:** `conf` distinto entre PC, o la dirección base (0x600000000000) ocupada.
- **El nodo 0 no ve a los demás / se queda en "waiting for N nodes":** puertos bloqueados o IP equivocada en `hosts`; repetir §2.
- **Se aborta con `DSM_ETIMEOUT`:** el home de la página no respondió en `request_timeout_ms` (3 s); revisar red y logs de ambos nodos.
- **Se aborta con `DSM_ENODEDOWN`:** el nodo dueño de esa página cayó o salió; es el comportamiento documentado (páginas LOST).
- **`DSM_ENOMEM` al asignar:** subir `node_memory` (igual en todas) o bajar el tamaño del problema.
- Guardar siempre los `logs/` de cada PC para el informe.
- Para reiniciar una corrida: terminar todos los procesos y volver a arrancar con el nodo 0 primero (un reinicio parcial consume IDs; `max_nodes` los limita).
