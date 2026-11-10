# KADP-DSM

Sistema de Memoria Compartida Distribuida (DSM) en espacio de usuario para Linux, en C++17 sobre POSIX.
Proyecto de IC-6600 Principios de Sistemas Operativos (TEC, II Semestre 2026).

Diseño completo: [docs/](docs/).

## Estructura

| Carpeta    | Contenido                                              |
|------------|--------------------------------------------------------|
| `include/` | Cabeceras públicas (`dsm.h`) e internas (`kadp_*.h`)   |
| `src/`     | Implementación de `libdsm` (un `.cpp` por módulo)      |
| `apps/`    | Aplicaciones de prueba que usan el API                 |
| `tests/`   | Pruebas unitarias y de integración                     |
| `conf/`    | `dsm.conf` de ejemplo                                  |
| `docs/`    | Documento de diseño (Entregable 1)                     |
