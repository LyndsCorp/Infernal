# Aro Infernal
*Aro Infernal* es un lenguaje de programación cuyo intérprete está escrito en *C*. Combina ideas de *Lua*, *Bash* y *Python*:
- *Lua*: súper fácil crear bloques.
- *Bash*: súper fácil ejecutar comandos del shell.
- *Python*: súper fácil importar librerías.

Si necesitas documentación, entra a:
https://github.com/LyndsCorp/Infernal-Documentation

Para compilar el intérprete, solo tienes que poner en la terminal:
``` Shell
make release
```
Y ya se crea el intérprete como el archivo llamado `infernal`.
Puedes borrar la carpeta `build` después de compilarlo; solo se utiliza durante la compilación.

## Desarrollo

Ejecuta `make help` para recibir ayuda de las formas de compilaciión y un poco de las estadísticas de tu repo.


## Cómo personalizar/adaptar el intérprete
Personalizar el intérprete adaptándolo a tu aplicación es sencillo. En la carpeta llamada `config` tienes todas las configuraciones.
``` Tree
config/
└── infernal/
    ├── bins/
    │   └── ALGO
    └── fire/
        └── ALGO-lib.fire

```
En `bins` se encuentran los binarios embebidos en el intérprete de *Infernal*. Resulta muy útil cuando no puedes implementar algo con *Infernal* puro y necesitas hacerlo en *C* o en cualquier lenguaje de programación (se pueden meter scripts).
En `fire` se encuentran las librerías `.fire`. Estas contienen funciones escritas en *Infernal*. En un script en *Infernal*, se llaman así:
``` Infernal
import ALGO-lib

ALGO-lib.funcion()
```
Igual que en *Python*, básicamente.
El archivo `.fire` debe tener un nombre distinto al del bin, por eso en el ejemplo usé `ALGO-lib.fire`.

Lo de embeber binarios sirve principalmente para esto:
``` Infernal Fire
# Ejemplo de una librería .fire

function funcion()
    !ALGO! # Como usa estos signos de exclamación, ejecuta el binario embebido.
fi
```


## Comparación con otros lenguajes
| Característica                            | Bash                                | Python                    | Lua                     | Rust                              | Go                       | **Infernal**                                                                  |
| ----------------------------------------- | ----------------------------------- | ------------------------- | ----------------------- | --------------------------------- | ------------------------ | ----------------------------------------------------------------------------- |
| **Tipado estático/fijo**                  | ❌                                   | ❌                         | ❌                       | ✅                                 | ✅                        | ✅ `int y = 5`                                                                 |
| **Inferencia de tipos**                   | ⚠️ Limitada                         | ⚠️ Dinámica               | ⚠️ Dinámica             | ✅                                 | ✅                        | ✅ `x = 10`                                                                    |
| **Integración con shell**                 | ✅ Nativa                            | ⚠️ Mediante APIs          | ⚠️ Mediante APIs        | ⚠️ Mediante APIs                  | ⚠️ Mediante APIs         | ✅ Directa                                                                     |
| **Bytecode / VM**                         | ❌ (`fi, done, esac`)                                  | ✅                         | ✅                       | ❌                                 | ❌                        | ✅                                                                             |
| **Cierre de bloques igual**            | ❌                                   | ✅ (indentación)                        | ✅ (`end`)                  | ✅ (`}`)                                 | ✅ (`}`)                    | ✅ (`fi`)                                                         |
| **Herramientas de diagnóstico/debugging** | ⚠️                                  | ✅ `pdb`                   | ⚠️                      | ⚠️ `dbg!` / herramientas externas | ⚠️ Herramientas externas | ✅ `printAllVars()`, `vartype()`                                               |
| **Embebible**                             | ⚠️                                  | ✅                         | ✅                       | ✅                                 | ⚠️                       | ✅ Diseñado para ser embebido                                                  |
| **Operadores `++` / `--`**                | ❌                                   | ❌                         | ❌                       | ✅                                 | ❌                        | ✅                                                                             |
| **Operaciones con listas**                | ⚠️                                  | ✅                         | ⚠️                      | ✅                                 | ⚠️                       | ✅                                                                             |
| **Funciones de strings**                  | ✅                                   | ✅                         | ✅                       | ✅                                 | ✅                        | ✅ `head`, `tail`, `starts`, `ends`, `has`, `count`, `replace`, `trim`, `join` |
| **Mapas / diccionarios**                  | ⚠️ Arrays asociativos               | ✅                         | ✅                       | ✅ `HashMap`                       | ✅ `map`                  | ✅ `keys()`, `values()`, `has()`, `delete()`, `size()`                         |
| **Soporte UTF-8**                         | ⚠️ Depende del entorno              | ✅                         | ⚠️                      | ✅                                 | ✅                        | ✅ `binbytes`, `hexbytes`, `utf8bytes`, `unicodeCodepoints`                    |
| **Colores en terminal**                   | ⚠️ `tput` / escapes                 | ⚠️ Librerías              | ⚠️ Librerías            | ⚠️ Crates                         | ⚠️ Paquetes              | ✅ `color()`, `success()`, `error()`, `warn()`                                 |
| **Secuencias de escape**                  | ✅                                   | ✅                         | ✅                       | ✅                                 | ✅                        | ✅ `\n`, `\t`, `\r`, `\N`, `\"`, `\'`, `\\`                                    |
| **Try / Catch**                           | ❌                                   | ✅                         | ❌                       | ❌ `Result` / `panic!`             | ❌                        | ✅ `try` / `catch`                                                             |
| **Bucles `for-in`**                       | ⚠️ `for ... in`                     | ✅                         | ✅                       | ✅                                 | ✅ `for ... range`        | ✅ `for nombre in nombres then`                                                |
| **Bucles `for` tradicionales**            | ⚠️                                  | ✅                         | ✅                       | ✅                                 | ✅                        | ✅ `for i, i < x, i++ then`                                                    |
| **Switch / selección múltiple**           | ⚠️ `case`                           | ⚠️ `match` (Python 3.10+) | ❌                       | ✅ `match`                         | ✅ `switch`               | ✅ `switch` / `case` / `default` / `break`                                     |
| **Scopes local/global**                   | ⚠️                                  | ✅                         | ✅                       | ✅                                 | ✅                        | ✅ `local`, `global` y `normal`                                                |
| **Manejo de errores**                     | ⚠️ Códigos de salida                | ✅ Excepciones             | ⚠️ `pcall` / `xpcall`   | ✅ `Result` / `panic!`             | ⚠️ Valores `error`       | ✅ `try` / `catch`                                                             |
| **Librería estándar amplia**              | ⚠️ Builtins + herramientas externas | ✅                         | ⚠️ Minimalista          | ✅                                 | ✅                        | ⚠️ En desarrollo                                                              |
| **Gestor de paquetes oficial/integrado**  | ❌                                   | ⚠️ `pip`                  | ⚠️ LuaRocks             | ✅ Cargo                           | ✅ Go Modules             | ❌                                             |
| **Tiempo de ejecución**                   | Interpretado                        | Interpretado / bytecode   | Interpretado / bytecode | Compilado a nativo                | Compilado a nativo       | **Interpretado con Bytecode + VM**                                                             |
| **Compilación a código nativo**           | ❌                                   | ❌                         | ❌                       | ✅                                 | ✅                        | ❌                                                                             |
| **Rendimiento**                           | Bajo–medio                          | Medio                     | Medio–alto              | **Muy alto**                      | **Muy alto**             | **Medio**                                                                      |
| **Curva de aprendizaje**                  | Media                               | Baja                      | Baja–media              | Alta                              | Media–baja               | **Baja–media**                                                                |
