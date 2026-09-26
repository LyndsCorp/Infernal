# Makefile para Infernal (intérprete modular)
# Uso:
#   make          - compila el intérprete y sus módulos .fire / bins / lava embebidos
#   make debug    - compila con logs de depuración (-DDEBUG)
#   make release  - compila optimizado para distribución (-O2, sin debug)
#   make min      - igual que 'make' pero sin las Lava embebidas
#   make minrelease - igual que 'make release' pero sin las Lava embebidas
#   make config   - crea/edita los metadatos (VERSION, HELP, WELCOME, EDITION)
#   make test     - compila Infernal si es necesario y ejecuta los demos uno por uno
#   make clean    - elimina objetos y ejecutable (NO borra nada en config/)
#   make sanitize - compila Infernal con AddressSanitizer y UBSan
#   make install      - instala el binario de Infernal
#   make install-lava - instala los módulos Lava (cabecera + .so)
#   make help     - muestra esta ayuda
# --------------------------------------------------------------------
# Configuración
# --------------------------------------------------------------------
CC       := gcc

# libffi: sin pkg-config. Se pueden sobrescribir desde el entorno o la línea de comandos.
FFI_CFLAGS ?=
FFI_LIBS   ?= -lffi

# Por defecto: con debug info, sin logs
CFLAGS   := -Wall -Wextra -g -std=c11 -D_GNU_SOURCE $(FFI_CFLAGS)
LDFLAGS  := $(FFI_LIBS) -ldl -lm -rdynamic
INCDIRS  := -Isrc

SRCDIR       := src
BUILDDIR     := build
FIRE_SRC_DIR := config/infernal/fire
BIN_SRC_DIR  := config/infernal/bins
LAVA_EMBED_DIR := config/infernal/lava
FIRE_GEN_DIR := $(BUILDDIR)/gen_fire
BIN_GEN_DIR  := $(BUILDDIR)/gen_bins
LAVA_GEN_DIR := $(BUILDDIR)/gen_lava
LAVA_DIR     := lava
LAVA_BUILD   := $(BUILDDIR)/lava
META_DIR     := src/metadata
TARGET       := infernal

PREFIX ?= /
BINDIR ?= $(PREFIX)usr/bin
LAVA_SYSDIR ?= $(PREFIX)usr/share/infernal/lava
LAVA_USERDIR := $$HOME/.infernal/lava

# MIN=1 desactiva la inclusión de Lava embebidas
MIN ?= 0

# --------------------------------------------------------------------
# Valores por defecto
# --------------------------------------------------------------------
GZIP_EMBEDDED := 1

# --------------------------------------------------------------------
# Colores ANSI (respeta NO_COLOR; si está definida, aunque sea vacía,
# se desactivan los colores)
# --------------------------------------------------------------------
ifeq ($(origin NO_COLOR), undefined)
  RED    := \033[1;31m
  GREEN  := \033[1;32m
  YELLOW := \033[1;33m
  BLUE   := \033[1;34m
  MAGENTA:= \033[1;35m
  CYAN   := \033[1;36m
  BOLD   := \033[1m
  RESET  := \033[0m
else
  RED    :=
  GREEN  :=
  YELLOW :=
  BLUE   :=
  MAGENTA:=
  CYAN   :=
  BOLD   :=
  RESET  :=
endif

# --------------------------------------------------------------------
# Leer configuración solo si estamos compilando (no en clean/help/...)
# --------------------------------------------------------------------
ifeq ($(filter clean distclean help config,$(MAKECMDGOALS)),)
  GZIP_CONFIG := config/gzip-embedded.bool
  ifeq ($(wildcard $(GZIP_CONFIG)),)
    $(warning Archivo $(GZIP_CONFIG) no encontrado. Se usará compresión gzip por defecto (true).)
  else
    GZIP_VAL := $(shell cat $(GZIP_CONFIG) | tr '[:upper:]' '[:lower:]')
    ifeq ($(GZIP_VAL),true)
      GZIP_EMBEDDED := 1
    else ifeq ($(GZIP_VAL),false)
      GZIP_EMBEDDED := 0
    else
      $(warning $(GZIP_CONFIG) contiene '$(GZIP_VAL)' en lugar de 'true' o 'false'. Se usará compresión gzip por defecto (true).)
      GZIP_EMBEDDED := 1
    endif
  endif

  ifeq ($(GZIP_EMBEDDED),1)
    $(info Compresión gzip de binarios embebidos: ACTIVADA)
    $(info La descompresión en ejecución usará zlib del sistema (carga dinámica).)
  else
    $(info Compresión gzip de binarios embebidos: DESACTIVADA)
  endif
  ifneq ($(MIN),1)
    $(info Lava embebidas detectadas: $(words $(wildcard $(LAVA_EMBED_DIR)/*.lava.c)) módulo(s))
  else
    $(info Lava embebidas: DESACTIVADAS (modo MIN))
  endif
endif

# --------------------------------------------------------------------
# Búsqueda automática de fuentes
# --------------------------------------------------------------------
SOURCES  := $(shell find $(SRCDIR) -name '*.c')
OBJECTS  := $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SOURCES))

# Módulos .fire (si existen)
FIRE_FILES    := $(wildcard $(FIRE_SRC_DIR)/*.fire)
FIRE_GEN_SRCS := $(patsubst $(FIRE_SRC_DIR)/%.fire, $(FIRE_GEN_DIR)/%.fire.c, $(FIRE_FILES))
FIRE_GEN_OBJS := $(FIRE_GEN_SRCS:.c=.o)

# Módulos binarios (bins)
BIN_FILES    := $(wildcard $(BIN_SRC_DIR)/*)
BIN_GEN_SRCS := $(patsubst $(BIN_SRC_DIR)/%, $(BIN_GEN_DIR)/%.c, $(BIN_FILES))
BIN_GEN_OBJS := $(BIN_GEN_SRCS:.c=.o)

# Librerías Lava (dev, no embebidas)
LAVA_FILES := $(wildcard $(LAVA_DIR)/*.c)
LAVA_SOS   := $(patsubst $(LAVA_DIR)/%.c, $(LAVA_BUILD)/%.lava, $(LAVA_FILES))

# Lava embebidas (solo si MIN != 1)
ifneq ($(MIN),1)
LAVA_EMBED_SRCS   := $(wildcard $(LAVA_EMBED_DIR)/*.lava.c)
LAVA_EMBED_SOS    := $(patsubst $(LAVA_EMBED_DIR)/%.lava.c, $(LAVA_GEN_DIR)/%.lava, $(LAVA_EMBED_SRCS))
LAVA_EMBED_GEN    := $(LAVA_EMBED_SOS:.lava=.lava.c)
LAVA_EMBED_OBJS   := $(LAVA_EMBED_GEN:.c=.o)
LAVA_EMBED_TABLE_SRC := $(BUILDDIR)/embedded_lava_table.c
LAVA_EMBED_TABLE_OBJ := $(LAVA_EMBED_TABLE_SRC:.c=.o)
endif

# Tabla de módulos embebidos Fire+Bins (auto-generada)
EMBED_TABLE_SRC := $(BUILDDIR)/embedded_table.c
EMBED_TABLE_OBJ := $(EMBED_TABLE_SRC:.c=.o)

# Metadatos embebidos
META_FILES    := VERSION HELP WELCOME EDITION
META_SRCS     := $(patsubst %, $(BUILDDIR)/metadata_%.c, $(META_FILES))
META_OBJS     := $(META_SRCS:.c=.o)
META_HUB_SRC  := $(BUILDDIR)/metadata.c
META_HUB_OBJ  := $(META_HUB_SRC:.c=.o)

# Todos los objetos finales
ALL_OBJS := $(OBJECTS) $(EMBED_TABLE_OBJ) $(META_OBJS) $(META_HUB_OBJ)
ifneq ($(FIRE_FILES),)
ALL_OBJS += $(FIRE_GEN_OBJS)
endif
ifneq ($(BIN_FILES),)
ALL_OBJS += $(BIN_GEN_OBJS)
endif
ifneq ($(MIN),1)
ifneq ($(LAVA_EMBED_SRCS),)
ALL_OBJS += $(LAVA_EMBED_OBJS) $(LAVA_EMBED_TABLE_OBJ)
endif
endif

# Archivos de dependencias automáticas
DEPS := $(ALL_OBJS:.o=.d)

# --------------------------------------------------------------------
# Reglas principales
# --------------------------------------------------------------------
.PHONY: all check-tools check-ffi clean help test regression sanitize debug release config install re lava min minrelease install-lava

all: check-tools check-ffi $(TARGET)

min:
	$(MAKE) MIN=1

minrelease:
	$(MAKE) MIN=1 CFLAGS='$(CFLAGS) -O2 -DNDEBUG' LDFLAGS='$(LDFLAGS) -s'

# --------------------------------------------------------------------
# Comprobación de herramientas y libffi
#
# Se rodean de una línea en blanco al principio y al final para que
# queden visualmente separados del resto de la salida de compilación.
# --------------------------------------------------------------------
check-tools:
	@echo ""
	@printf "$(CYAN)[CHECK]$(RESET) herramientas... "
	@for tool in $(firstword $(CC)) od sed tr mkdir basename cat; do \
		if ! command -v $$tool >/dev/null 2>&1; then \
			printf "$(RED)FALTA$(RESET)\n"; \
			printf "$(RED)Error: falta la herramienta '$$tool' necesaria para compilar.$(RESET)\n"; \
			exit 1; \
		fi; \
	done; \
	if [ "$(GZIP_EMBEDDED)" = "1" ]; then \
		if ! command -v gzip >/dev/null 2>&1; then \
			printf "$(RED)FALTA$(RESET)\n"; \
			printf "$(RED)Error: gzip es necesario para comprimir los binarios embebidos.$(RESET)\n"; \
			printf "$(YELLOW)Puedes desactivar la compresión con GZIP_EMBEDDED=0 o instalando gzip.$(RESET)\n"; \
			exit 1; \
		fi; \
	fi; \
	printf "$(GREEN)OK$(RESET)\n"

check-ffi:
	@printf "$(CYAN)[CHECK]$(RESET) libffi... "
	@mkdir -p $(BUILDDIR)
	@tmp=$(BUILDDIR)/.ffi-check-$$$$; \
	if printf '#include <ffi.h>\nint main(void){return 0;}\n' | $(CC) $(CFLAGS) -x c - $(FFI_LIBS) -o $$tmp >/dev/null 2>&1; then \
		rm -f $$tmp; \
		printf "$(GREEN)OK$(RESET)\n"; \
		echo ""; \
	else \
		rm -f $$tmp; \
		printf "$(RED)FALTA$(RESET)\n"; \
		printf "$(RED)Error: Infernal necesita libffi con sus cabeceras de desarrollo (ffi.h) y biblioteca enlazable.$(RESET)\n"; \
		printf "$(RED)Instálalo con el paquete de desarrollo de libffi de tu distribución.$(RESET)\n"; \
		printf "$(YELLOW)Si libffi está en una ruta no estándar, exporta FFI_CFLAGS y FFI_LIBS.$(RESET)\n"; \
		exit 1; \
	fi

debug:
	$(MAKE) CFLAGS='$(CFLAGS) -DDEBUG'

release:
	$(MAKE) CFLAGS='$(CFLAGS) -O2 -DNDEBUG' LDFLAGS='$(LDFLAGS) -s'

$(TARGET): $(ALL_OBJS)
	@printf "$(CYAN)[LD]$(RESET)   $@\n"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@printf "$(GREEN)✓$(RESET) Binario generado: $(BOLD)$@$(RESET)\n"

# Compilación de fuentes del proyecto
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@printf "$(CYAN)[CC]$(RESET)   $<\n"
	@$(CC) $(CFLAGS) $(INCDIRS) -MMD -MP -c $< -o $@

# --------------------------------------------------------------------
# Reglas para módulos .fire (sin comprimir)
# --------------------------------------------------------------------
ifneq ($(FIRE_FILES),)
$(FIRE_GEN_DIR)/%.fire.c: $(FIRE_SRC_DIR)/%.fire
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[OD]$(RESET)   $< $(BOLD)→$(RESET) $@\n"
	@name=$$(basename $@ .fire.c); \
	sanename=$$(echo $${name} | tr '-' '_'); \
	echo "unsigned char config_infernal_fire_$${sanename}[] = {" > $@; \
	od -A n -t x1 -v < "$<" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	echo "};" >> $@; \
	echo "unsigned int config_infernal_fire_$${sanename}_len = sizeof(config_infernal_fire_$${sanename});" >> $@

$(FIRE_GEN_DIR)/%.o: $(FIRE_GEN_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "$(CYAN)[CC]$(RESET)   $< $(MAGENTA)(embedded fire)$(RESET)\n"
	@$(CC) $(CFLAGS) -c $< -o $@
endif

# --------------------------------------------------------------------
# Reglas para módulos binarios (bins) con compresión opcional
# --------------------------------------------------------------------
ifneq ($(BIN_FILES),)
$(BIN_GEN_DIR)/%.c: $(BIN_SRC_DIR)/%
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[BIN]$(RESET)  $< $(BOLD)→$(RESET) $@\n"
	@name=$$(basename $<); \
	sanename=$$(echo $${name} | tr '-' '_'); \
	if [ $(GZIP_EMBEDDED) -eq 1 ]; then \
	  echo "unsigned char config_infernal_bins_$${sanename}[] = {" > $@; \
	  gzip -c < "$<" | od -A n -t x1 -v | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	  echo "};" >> $@; \
	  echo "unsigned int config_infernal_bins_$${sanename}_len = sizeof(config_infernal_bins_$${sanename});" >> $@; \
	else \
	  echo "unsigned char config_infernal_bins_$${sanename}[] = {" > $@; \
	  od -A n -t x1 -v < "$<" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	  echo "};" >> $@; \
	  echo "unsigned int config_infernal_bins_$${sanename}_len = sizeof(config_infernal_bins_$${sanename});" >> $@; \
	fi

$(BIN_GEN_DIR)/%.o: $(BIN_GEN_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "$(CYAN)[CC]$(RESET)   $< $(MAGENTA)(embedded bin)$(RESET)\n"
	@$(CC) $(CFLAGS) -c $< -o $@
endif

# --------------------------------------------------------------------
# Reglas para Lava EMBEBIDAS (config/infernal/lava/*.lava.c)
#
# Flujo:
#   config/infernal/lava/foo.lava.c  (C fuente del módulo Lava)
#        |  $(CC) -fPIC -shared -I$(LAVA_DIR)
#        v
#   build/gen_lava/foo.lava           (shared object)
#        |  od -> bytes
#        v
#   build/gen_lava/foo.lava.c         (array unsigned char)
#        |  $(CC)
#        v
#   build/gen_lava/foo.lava.o         (objeto enlazado en el binario)
#
# En runtime se cargan con memfd_create + /proc/self/fd/N (Linux).
# --------------------------------------------------------------------
ifneq ($(MIN),1)
ifneq ($(LAVA_EMBED_SRCS),)
$(LAVA_GEN_DIR)/%.lava: $(LAVA_EMBED_DIR)/%.lava.c
	@mkdir -p $(dir $@)
	@printf "$(MAGENTA)[LAVA-EMB]$(RESET) $< $(BOLD)→$(RESET) $@\n"
	@$(CC) $(CFLAGS) -fPIC -shared -I$(LAVA_DIR) -o $@ $<

$(LAVA_GEN_DIR)/%.lava.c: $(LAVA_GEN_DIR)/%.lava
	@printf "$(MAGENTA)[LAVA-GEN]$(RESET) $< $(BOLD)→$(RESET) $@\n"
	@name=$$(basename $@ .lava.c); \
	sanename=$$(echo $${name} | tr '-' '_'); \
	echo "unsigned char config_infernal_lava_$${sanename}[] = {" > $@; \
	od -A n -t x1 -v < "$<" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	echo "};" >> $@; \
	echo "unsigned int config_infernal_lava_$${sanename}_len = sizeof(config_infernal_lava_$${sanename});" >> $@

$(LAVA_GEN_DIR)/%.o: $(LAVA_GEN_DIR)/%.c
	@printf "$(CYAN)[CC]$(RESET)   $< $(MAGENTA)(lava-emb)$(RESET)\n"
	@$(CC) $(CFLAGS) $(INCDIRS) -c $< -o $@

$(LAVA_EMBED_TABLE_SRC): $(LAVA_EMBED_SOS)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[GEN]$(RESET)  $@\n"
	@echo '// Auto-generated embedded Lava module table' > $@
	@echo '#include <stddef.h>' >> $@
	@echo '#include "embedded/embedded.h"' >> $@
	@for f in $(LAVA_EMBED_SRCS); do \
		name=$$(basename $$f .lava.c); \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "extern unsigned char config_infernal_lava_$${sanename}[];" >> $@; \
		echo "extern unsigned int config_infernal_lava_$${sanename}_len;" >> $@; \
	done
	@echo '' >> $@
	@echo 'EmbeddedLavaModule embedded_lava_modules[] = {' >> $@
	@for f in $(LAVA_EMBED_SRCS); do \
		name=$$(basename $$f .lava.c); \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "  {\"$$name\", config_infernal_lava_$${sanename}, &config_infernal_lava_$${sanename}_len}," >> $@; \
	done
	@echo '  {NULL, NULL, NULL}' >> $@
	@echo '};' >> $@

$(LAVA_EMBED_TABLE_OBJ): $(LAVA_EMBED_TABLE_SRC)
	@printf "$(CYAN)[CC]$(RESET)   $<\n"
	@$(CC) $(CFLAGS) $(INCDIRS) -c $< -o $@
endif
endif

# --------------------------------------------------------------------
# Reglas para librerías Lava dev (lava/*.c -> build/lava/*.lava)
# --------------------------------------------------------------------
ifneq ($(LAVA_FILES),)
$(LAVA_BUILD)/%.lava: $(LAVA_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "$(MAGENTA)[LAVA]$(RESET) $< $(BOLD)→$(RESET) $@\n"
	@$(CC) $(CFLAGS) -fPIC -shared -I$(LAVA_DIR) -o $@ $<
endif

# --------------------------------------------------------------------
# Tabla de módulos embebidos Fire+Bins (con indicador de compresión)
# --------------------------------------------------------------------
$(EMBED_TABLE_SRC): $(FIRE_FILES) $(BIN_FILES)
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[GEN]$(RESET)  $@\n"
	@echo '// Auto-generated embedded module table' > $@
	@echo '#include <stddef.h>' >> $@
	@echo '#include "embedded/embedded.h"' >> $@
	@for f in $(FIRE_FILES); do \
		name=$$(basename $$f .fire); \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "extern unsigned char config_infernal_fire_$${sanename}[];" >> $@; \
		echo "extern unsigned int config_infernal_fire_$${sanename}_len;" >> $@; \
	done
	@for f in $(BIN_FILES); do \
		name=$$(basename $$f); \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "extern unsigned char config_infernal_bins_$${sanename}[];" >> $@; \
		echo "extern unsigned int config_infernal_bins_$${sanename}_len;" >> $@; \
	done
	@echo '' >> $@
	@echo 'EmbeddedModule embedded_modules[] = {' >> $@
	@for f in $(FIRE_FILES); do \
		name=$$(basename $$f .fire); \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "  {\"$$name\", config_infernal_fire_$${sanename}, &config_infernal_fire_$${sanename}_len, 0}," >> $@; \
	done
	@for f in $(BIN_FILES); do \
		name=$$(basename $$f); \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "  {\"$$name\", config_infernal_bins_$${sanename}, &config_infernal_bins_$${sanename}_len, $(GZIP_EMBEDDED)}," >> $@; \
	done
	@echo '  {NULL, NULL, NULL, 0}' >> $@
	@echo '};' >> $@

$(EMBED_TABLE_OBJ): $(EMBED_TABLE_SRC)
	@printf "$(CYAN)[CC]$(RESET)   $<\n"
	@$(CC) $(CFLAGS) $(INCDIRS) -c $< -o $@

# --------------------------------------------------------------------
# Reglas para metadatos
# --------------------------------------------------------------------
$(BUILDDIR)/metadata_%.c: $(META_DIR)/%
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[META]$(RESET) $<\n"
	@name=$*; \
	sanename=$$(echo $${name} | tr '-' '_'); \
	echo "unsigned char metadata_$${sanename}[] = {" > $@; \
	od -A n -t x1 -v < "$<" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	echo "0x00 };" >> $@; \
	echo "unsigned int metadata_$${sanename}_len = sizeof(metadata_$${sanename}) - 1;" >> $@

$(BUILDDIR)/metadata_%.o: $(BUILDDIR)/metadata_%.c
	@printf "$(CYAN)[CC]$(RESET)   $< $(MAGENTA)(metadata)$(RESET)\n"
	@$(CC) $(CFLAGS) -c $< -o $@

$(META_HUB_SRC): $(patsubst %, $(META_DIR)/%, $(META_FILES))
	@mkdir -p $(dir $@)
	@printf "$(BLUE)[GEN]$(RESET)  $@\n"
	@echo '// Auto-generated embedded metadata hub' > $@
	@echo '#include <string.h>' >> $@
	@echo '#include <stddef.h>' >> $@
	@for f in $(META_FILES); do \
		name=$$f; \
		sanename=$$(echo $${name} | tr '-' '_'); \
		echo "extern unsigned char metadata_$${sanename}[];" >> $@; \
		echo "extern unsigned int metadata_$${sanename}_len;" >> $@; \
	done
	@echo '' >> $@
	@echo 'const char* get_metadata(const char *type) {' >> $@
	@echo '    if (strcmp(type, "VERSION") == 0) return (const char*)metadata_VERSION;' >> $@
	@echo '    if (strcmp(type, "HELP") == 0) return (const char*)metadata_HELP;' >> $@
	@echo '    if (strcmp(type, "WELCOME") == 0) return (const char*)metadata_WELCOME;' >> $@
	@echo '    if (strcmp(type, "EDITION") == 0) return (const char*)metadata_EDITION;' >> $@
	@echo '    return NULL;' >> $@
	@echo '}' >> $@

$(META_HUB_OBJ): $(META_HUB_SRC)
	@printf "$(CYAN)[CC]$(RESET)   $<\n"
	@$(CC) $(CFLAGS) $(INCDIRS) -c $< -o $@

# Incluir dependencias automáticas si existen
-include $(DEPS)

# --------------------------------------------------------------------
# Objetivo config: crea metadatos si faltan y los abre con el editor
# --------------------------------------------------------------------
config:
	@mkdir -p $(META_DIR)
	@for f in $(META_FILES); do \
		if [ ! -f $(META_DIR)/$$f ]; then \
			printf "$(YELLOW)Creando $(META_DIR)/$$f con contenido por defecto...$(RESET)\n"; \
			case $$f in \
				VERSION) echo "1.x" > $(META_DIR)/$$f ;; \
				EDITION) echo "Infernal 1.x (Rama)\n──────────────────────────────────────────────\n\nVersión:           1.x\nEdición:           Para tu app\nArquitectura:      La de tu PC (puedes verlo con uname -m)\nSistema operativo: POSIX (Linux, macOS)\n\nDistribuido por:   Tu Nombre O Apodo\nProyecto:          Tu proyecto\nLicencia:          GPL v3+\n\nCódigo fuente:        https://github.com/tu-user/tu-repo-fork/tree/rama-release\nRepositorio:          https://github.com/tu-user/tu-repo-fork\nRepositorio oficial:  https://github.com/LyndsCorp/Infernal\nDocumentación:        https://github.com/LyndsCorp/Infernal-Documentation\n\nCopyright (C) AÑO Tu empresa/organizacion/nombre" > $(META_DIR)/$$f ;; \
				HELP) echo "Infernal - lenguaje de scripting inspirado en Bash + Lua + Python.\n\nUso:\n  infernal <script.inf> [flags...]\n\nOpciones:\n  --help       Muestra esta ayuda.\n  --version    Muestra la versión del intérprete.\n  --edition    Muestra la edición del intérprete.\n\nSin argumentos muestra el mensaje de bienvenida.\n\nDocumentación: https://github.com/LyndsCorp/Infernal-Documentation" > $(META_DIR)/$$f ;; \
				WELCOME) echo "¡Bienvenido a Infernal!\n\nEscribe infernal --help para recibir ayuda.\n\nInfernal es un lenguaje de programación inspirado en Bash + Lua + Python.\n\nCopyright (C), GPL v3+, Lynds Corp." > $(META_DIR)/$$f ;; \
			esac; \
		fi; \
	done
	@printf "$(CYAN)Abriendo metadatos para editar...$(RESET)\n"
	@if [ -z "$$EDITOR" ]; then EDITOR=nano; fi; \
	$$EDITOR $(META_DIR)/VERSION $(META_DIR)/EDITION $(META_DIR)/HELP $(META_DIR)/WELCOME

# --------------------------------------------------------------------
# Limpieza
# --------------------------------------------------------------------
clean:
	@printf "$(RED)[CLEAN]$(RESET)\n"
	@rm -rf $(BUILDDIR) $(TARGET)
	@echo "rm -rf build infernal"
	@printf "$(GREEN)✓$(RESET) Objetos y binario eliminados.\n"

clean-lava:
	@printf "$(RED)[CLEAN]$(RESET)\n"
	@rm -rf build/lava/ $(TARGET)
	@echo "rm -rf build/lava/"
	@printf "$(GREEN)✓$(RESET) Lava limpiados.\n"

# --------------------------------------------------------------------
# Ayuda
# --------------------------------------------------------------------
help:
	@printf "$(BOLD)Infernal Makefile$(RESET)\n"
	@printf "$(BOLD)-----------------$(RESET)\n"
	@printf "$(BOLD)Objetivos:$(RESET)\n"
	@printf "             : (vacío; solo ejecuta 'make') hace lo mismo que 'make all'\n"
	@printf "  $(GREEN)all$(RESET)        : compila el intérprete (por defecto)\n"
	@printf "  $(GREEN)min$(RESET)        : igual que 'all' pero sin las Lava embebidas\n"
	@printf "  $(GREEN)clean$(RESET)      : elimina objetos (build/) y el ejecutable (infernal) (no toca config/)\n"
	@printf "  $(GREEN)debug$(RESET)      : compila con soporte de depuración (-DDEBUG)\n"
	@printf "  $(GREEN)release$(RESET)    : compila optimizado para distribución (-O2, sin debug)\n"
	@printf "  $(GREEN)minrelease$(RESET) : igual que 'release' pero sin las Lava embebidas\n"
	@printf "  $(GREEN)config$(RESET)     : crea o edita los metadatos (VERSION, HELP, WELCOME, EDITION)\n"
	@printf "  $(GREEN)test$(RESET)       : compila Infernal si es necesario y ejecuta los demos uno por uno\n"
	@printf "  $(GREEN)sanitize$(RESET)   : compila Infernal con AddressSanitizer y UBSan\n"
	@printf "  $(GREEN)lava$(RESET)       : compila solo las librerías Lava de desarrollo (lava/*.c -> build/lava/*.lava)\n"
	@printf "  $(GREEN)help$(RESET)       : muestra esta ayuda\n"
	@echo
	@echo
	@printf "$(BOLD)Instalación:$(RESET)\n"
	@printf "  $(GREEN)make install$(RESET)                       : instala el binario de Infernal para el usuario\n"
	@printf "  $(GREEN)sudo make install$(RESET)                  : instala el binario de Infernal para el sistema\n"
	@printf "  $(GREEN)make install-lava$(RESET)                  : instala los módulos Lava (cabecera + .so) para el usuario\n"
	@printf "  $(GREEN)sudo make install-lava$(RESET)             : instala los módulos Lava (cabecera + .so) para el sistema\n"
	@printf "  $(GREEN)sudo make install PREFIX=/mnt/ROOT/$(RESET): instala Infernal en un sitio personalizado\n"
	@echo
	@printf "· Sin sudo, se instala para el usuario.\n"
	@printf "· Con sudo, se instala para el sistema o el PREFIX indicado.\n"
	@printf "· Recuerda poner la / final en PREFIX.\n"
	@echo
	@echo
	@printf "$(BOLD)Estadísticas:$(RESET)\n"
	@printf "  Archivos fuente (.c): %d\n" $(words $(SOURCES))
	@printf "  Módulos .fire embebidos: %d\n" $(words $(FIRE_FILES))
	@printf "  Binarios embebidos: %d\n" $(words $(BIN_FILES))
	@printf "  Librerías Lava dev (.c): %d\n" $(words $(LAVA_FILES))
	@printf "  Librerías Lava embebidas (.lava.c): %d\n" $(words $(LAVA_EMBED_SRCS))
	@printf "  Versión: "
	@cat src/metadata/VERSION || echo "No disponible"
	@echo

test: $(TARGET)
	@for file in demos/*.inf; do \
		printf "$(CYAN)[TEST]$(RESET) $$file\n"; \
		TERM=$${TERM:-xterm} SHELL=$${SHELL:-/bin/sh} ./$(TARGET) $$file || exit 1; \
	done

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer' \
	        LDFLAGS='$(LDFLAGS) -fsanitize=address,undefined' all
	$(MAKE) test

lava: $(LAVA_SOS)

# --------------------------------------------------------------------
# Instalación del binario de Infernal (sin Lava)
# --------------------------------------------------------------------
install:
	$(MAKE) release
	@if [ "$$(id -u)" -eq 0 ]; then \
		printf "$(YELLOW)[INSTALL ROOT]$(RESET) $(BINDIR)/$(TARGET)\n"; \
		install -Dm755 $(TARGET) $(BINDIR)/$(TARGET); \
		printf "$(GREEN)✓ Infernal instalado para el sistema.$(RESET)\n"; \
	else \
		printf "$(GREEN)[INSTALL USER]$(RESET) $$HOME/.local/bin/$(TARGET)\n"; \
		mkdir -p "$$HOME/.local/bin"; \
		install -Dm755 $(TARGET) "$$HOME/.local/bin/$(TARGET)"; \
		printf "$(GREEN)✓ Infernal instalado para el usuario.$(RESET)\n"; \
		printf "$(YELLOW)Asegúrate de que $$HOME/.local/bin está en tu PATH.$(RESET)\n"; \
	fi

# --------------------------------------------------------------------
# Instalación de los módulos Lava (cabecera + .so)
#
# Antes de tocar nada, verificamos que TODOS los $(LAVA_SOS) existan.
# Si falta alguno, mostramos un error rojo llamativo y abortamos sin
# instalar nada. Así evitamos el feo 'install: cannot stat' a mitad.
# --------------------------------------------------------------------
install-lava:
	@missing=""; \
	for so in $(LAVA_SOS); do \
		if [ ! -f "$$so" ]; then \
			missing="$$missing $$so"; \
		fi; \
	done; \
	if [ -n "$$missing" ]; then \
		printf "\n$(RED)════════════════════════════════════════════════════════════════$(RESET)\n"; \
		printf "$(RED)  ✗  ERROR: faltan módulos Lava para instalar$(RESET)\n"; \
		printf "$(RED)════════════════════════════════════════════════════════════════$(RESET)\n\n"; \
		printf "Los siguientes archivos no existen:\n\n"; \
		for f in $$missing; do \
			printf "    $(RED)✗$(RESET)  %s\n" "$$f"; \
		done; \
		printf "\nCompila Infernal primero para generarlos:\n\n"; \
		printf "    $(YELLOW)make$(RESET)                       # compila el binario y los módulos\n"; \
		printf "    sudo $(YELLOW)make install-lava$(RESET)     # luego instálalos\n\n"; \
		exit 1; \
	fi; \
	if [ "$$(id -u)" -eq 0 ]; then \
		printf "$(YELLOW)[INSTALL ROOT]$(RESET) $(LAVA_SYSDIR)/\n"; \
		mkdir -p $(LAVA_SYSDIR); \
		install -Dm644 $(LAVA_DIR)/lava.h $(LAVA_SYSDIR)/lava.h; \
		for so in $(LAVA_SOS); do \
			printf "$(YELLOW)[INSTALL ROOT]$(RESET) $(LAVA_SYSDIR)/$$(basename $$so)\n"; \
			install -Dm644 $$so $(LAVA_SYSDIR)/$$(basename $$so); \
		done; \
		printf "$(GREEN)✓ Módulos Lava instalados para el sistema.$(RESET)\n"; \
	else \
		printf "$(GREEN)[INSTALL USER]$(RESET) $$HOME/.infernal/lava/\n"; \
		mkdir -p "$$HOME/.infernal/lava"; \
		install -Dm644 $(LAVA_DIR)/lava.h "$$HOME/.infernal/lava/lava.h"; \
		for so in $(LAVA_SOS); do \
			printf "$(GREEN)[INSTALL USER]$(RESET) $$HOME/.infernal/lava/$$(basename $$so)\n"; \
			install -Dm644 $$so "$$HOME/.infernal/lava/$$(basename $$so)"; \
		done; \
		printf "$(GREEN)✓ Módulos Lava instalados para el usuario.$(RESET)\n"; \
	fi

re:
	$(MAKE) clean
	$(MAKE)

rerelease:
	$(MAKE) clean
	$(MAKE) release
