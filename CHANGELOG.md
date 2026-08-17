# CHANGELOG

## 2026-08-17 — Hardening de parser, VM, ownership y sintaxis

### Declaraciones de variables

- Añadidas declaraciones tipadas múltiples en una misma línea.
- Todas las variables de una declaración múltiple deben compartir tipo y scope.
- Añadido soporte para `global TIPO ...` y `local TIPO ...`.
- Añadido soporte para `local nombre = valor` / `global nombre = valor` como declaración con tipado automático de una sola variable.
- Las variables declaradas sin valor reciben valores base:
  - `int` → `0`
  - `float` → `0.0`
  - `bool` → `false`
  - `string` → `""`
  - `list` → `[]`
  - `map` → `{}`
- Se rechazan las declaraciones múltiples sin tipo explícito.

### Variables y `$`

- En instrucciones de comando, `$` sigue siendo necesario para referenciar una variable de Infernal.
- En argumentos de funciones, `$` es opcional.
- Dentro de expresiones se permite mezclar referencias con y sin `$`, por ejemplo `$var + var2`.
- Se conserva `?VAR` para producir una referencia de variable del shell (`$VAR`) cuando corresponde.

### Concatenación

- `string + int` convierte el entero a texto.
- `string + float` convierte el float a texto.
- `string + bool` convierte el booleano a `true` / `false`.
- Se preserva la concatenación entre strings.

### Parser y errores

- Corregida la limpieza de AST parcial cuando el parser genera un error y salta mediante `longjmp`.
- Evitados leaks de nodos, listas, nombres, comandos y chunks creados antes de un error sintáctico.
- Evitado el `heap-use-after-free` producido cuando el parser limpiaba el AST y `main` intentaba liberarlo de nuevo.
- Evitado un `double free` causado por múltiples nodos del AST que compartían el mismo `varname`.

### `for`

- Corregido el ownership del nombre de variable de `for` para que los nodos `init` y `for_stmt` no compartan el mismo puntero.
- Corregida la limpieza de `for` ante errores de parsing.
- `for-in` libera la lista temporal evaluada y los temporales creados durante cada iteración.

### Ownership de `Value`

- Corregidas fugas al sustituir valores existentes en scopes.
- Corregidas fugas de temporales de `eval_index` y asignaciones indexadas.
- Corregidas fugas de claves/valores temporales de mapas.
- Corregidas fugas de operandos temporales en operaciones aritméticas y comparaciones de la VM.
- Corregida la liberación de operandos en `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`, `OP_NEG`, comparaciones, lógica y `OP_INDEX`.
- `eval_slice()` libera la lista fuente una vez creada la copia de resultado.
- Corregidas fugas de argumentos evaluados de funciones/builtins.
- Corregida la sustitución de valores en variables ya registradas por el compilador.

### Verificación

La build de sanitizer se ejecutó con AddressSanitizer + UndefinedBehaviorSanitizer y después se ejecutó:

```bash
SHELL=/bin/bash make test
```

Resultado: `RC=0`.

Se ejecutaron todos los demos definidos por `make test`, incluyendo:

- `barra-progreso.inf`
- `for-list.inf`
- `for.inf`
- `funciones-strings.inf`
- `hostname.inf`
- `list-to-string.inf`
- `list.inf`
- `maps.inf`
- `operadores.inf`
- `os-info.inf`
- `prints.inf`
- `rainbow.inf`
- `secuencias-escape.inf`
- `shell-ui.inf`
- `test-listas.inf`

No se reportaron errores de AddressSanitizer ni de LeakSanitizer en la ejecución completa.

### Nota sobre `SHELL`

`os-info.inf` utiliza `?SHELL` para que el shell del sistema expanda `$SHELL`. En el entorno de prueba usado para esta verificación la variable de entorno `SHELL` no estaba definida, por lo que la prueba completa se ejecutó con `SHELL=/bin/bash` para validar exactamente esa funcionalidad.
## 2026-08-17 — Expresiones booleanas como argumentos de funciones

- Las llamadas a funciones pueden recibir una expresión booleana completa como argumento, por ejemplo:
  `testear("...", a == 0 and b == 0 and c == 0)`
- Implementada la evaluación de los operadores lógicos `and` y `or` en el evaluator.
- Los operadores lógicos requieren operandos de tipo `bool` y producen un `Value` booleano (`true`/`false`).
- Los operandos temporales de `and`/`or` se liberan correctamente después de la evaluación.
- Verificado con `script_usado(1).inf` y con una regresión adicional que combina `and`, `or`, comparaciones y referencias con `$`.

### Verificación

`script_usado(1).inf` termina con `RC=0` y las pruebas 5–25, incluyendo:

```infernal
testear("Múltiples int globales declaradas correctamente", a == 0 and b == 0 and c == 0)
```

se evalúan correctamente como valores booleanos.

