# KADP-DSM

Sistema de Memoria Compartida Distribuida (DSM) en espacio de usuario para Linux, en C++17 sobre POSIX.
Proyecto de IC-6600 Principios de Sistemas Operativos (TEC, II Semestre 2026).

Diseño completo: [docs/](docs/).

## Estructura

| Carpeta    | Contenido                                              |
|------------|--------------------------------------------------------|
| `include/` | Cabeceras públicas (`dsm.h`) e internas (`kadp_*.h`)   |
| `src/`     | Implementación de `libdsm` (un `.cpp` por módulo)      |
| `apps/`    | Aplicaciones: `mm_dsm` (matrices), `relax_dsm` (relajación), `selftest_dsm`, `idle_dsm` |
| `tests/`   | Pruebas unitarias (`make test`) y de integración (`make itest`) |
| `conf/`    | `dsm.conf` de ejemplo                                  |
| `docs/`    | Documentos de diseño y entregables                     |

## Compilar y probar

```
make          # build/libdsm.a, pruebas y aplicaciones
make test     # pruebas unitarias
make itest    # integración con procesos en esta PC (selftest 3 y 4 nodos, apps, caída de un nodo)
```

## Aplicaciones

Todas se lanzan una vez por nodo, **el nodo 0 primero**; `<nodos>` es la cantidad total.

```
build/apps/mm_dsm conf/dsm.conf <N> <K> <M> 0 <nodos>            # C = A x B, verifica 200 entradas
build/apps/relax_dsm conf/dsm.conf <dim> <precision> <max_iter> 0 <nodos>  # relajación, verifica contra corrida secuencial
build/apps/selftest_dsm conf/dsm.conf <nodos>                     # escenarios de concurrencia, locks, nombres, free
build/apps/idle_dsm conf/dsm.conf <segundos> [plain|holder|waiter|alloc]
```

## Consola y vista web

Si stdin es una terminal (o `DSM_CONSOLE=1`) cada nodo atiende: `help`, `nodes`, `stats [global|<id>]`,
`pages local`, `pages global`, `page <id>`, `allocs`, `names`, `quit`.
Con `[web] enabled = true`, cada nodo sirve las mismas consultas en `http://<ip>:<port_base + 10*slot + 3>/`
(por ejemplo `http://localhost:28103/`).

## Laboratorio (varias PC)

1. Misma copia del repositorio compilada en cada PC (`make`), y el **mismo** `dsm.conf` en todas.
2. En `[cluster]`: `hosts = ip_pc1, ip_pc2, ...` y `max_nodes` = cantidad de nodos que se usarán.
   `node_memory` × nodos debe alojar los datos (el archivo, o A, B y C).
3. Los puertos `28100-28199` deben estar abiertos entre las PC (TCP). Verificar con `nc -vz <ip> 28101`.
4. Arrancar primero el nodo 0 y esperar ~1 s; luego los demás (cada PC ejecuta su nodo en el slot 0).
5. Los nodos se anuncian con la primera IPv4 que no es loopback; si la PC tiene varias interfaces,
   revisar `nodes` en la consola.
