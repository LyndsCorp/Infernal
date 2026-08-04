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
