# Makefile para Infernal (intérprete modular)
# Uso:
#   make          - compila el intérprete y sus módulos .fire embebidos
#   make debug    - compila con logs de depuración (-DDEBUG)
#   make release  - compila optimizado para distribución (-O2, sin debug)
#   make config   - crea/edita los metadatos (VERSION, HELP, WELCOME, EDITION)
#   make clean    - elimina objetos y ejecutable (NO borra nada en config/)
#   make distclean- limpia también los archivos generados de .fire y metadatos (NO borra config/)
#   make help     - muestra esta ayuda
# --------------------------------------------------------------------
# Configuración
# --------------------------------------------------------------------
CC       := gcc
# Por defecto: con debug info, sin logs
CFLAGS   := -Wall -Wextra -g -std=c11 -D_GNU_SOURCE
LDFLAGS  := -ldl
INCDIRS  := -Isrc

SRCDIR       := src
BUILDDIR     := build
FIRE_SRC_DIR := config/infernal/fire
BIN_SRC_DIR  := config/infernal/bins
FIRE_GEN_DIR := $(BUILDDIR)/gen_fire
BIN_GEN_DIR  := $(BUILDDIR)/gen_bins
META_DIR     := src/metadata
TARGET       := infernal

# --------------------------------------------------------------------
# Valores por defecto
# --------------------------------------------------------------------
GZIP_EMBEDDED := 1

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

# Tabla de módulos embebidos (auto-generada)
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

# Archivos de dependencias automáticas
DEPS := $(ALL_OBJS:.o=.d)

# --------------------------------------------------------------------
# Reglas principales
# --------------------------------------------------------------------
.PHONY: all clean distclean help test sanitize debug release config

all: $(TARGET)

debug:
	$(MAKE) CFLAGS='$(CFLAGS) -DDEBUG'

release:
	$(MAKE) CFLAGS='$(CFLAGS) -O2 -DNDEBUG' LDFLAGS='$(LDFLAGS) -s'

$(TARGET): $(ALL_OBJS)
	@echo " [LD] $@"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compilación de fuentes del proyecto
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo " [CC] $<"
	$(CC) $(CFLAGS) $(INCDIRS) -MMD -MP -c $< -o $@

# --------------------------------------------------------------------
# Reglas para módulos .fire (sin comprimir)
# --------------------------------------------------------------------
ifneq ($(FIRE_FILES),)
$(FIRE_GEN_DIR)/%.fire.c: $(FIRE_SRC_DIR)/%.fire
	@mkdir -p $(dir $@)
	@echo " [OD] $< -> $@"
	@name=$$(basename $@ .fire.c); \
	sanename=$$(echo $${name} | tr '-' '_'); \
	echo "unsigned char config_infernal_fire_$${sanename}[] = {" > $@; \
	od -A n -t x1 -v < "$<" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	echo "};" >> $@; \
	echo "unsigned int config_infernal_fire_$${sanename}_len = sizeof(config_infernal_fire_$${sanename});" >> $@

$(FIRE_GEN_DIR)/%.o: $(FIRE_GEN_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo " [CC] $< (embedded)"
	$(CC) $(CFLAGS) -c $< -o $@
endif

# --------------------------------------------------------------------
# Reglas para módulos binarios (bins) con compresión opcional
# --------------------------------------------------------------------
ifneq ($(BIN_FILES),)
$(BIN_GEN_DIR)/%.c: $(BIN_SRC_DIR)/%
	@mkdir -p $(dir $@)
	@echo " [BIN] $< -> $@"
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
	@echo " [CC] $< (embedded)"
	$(CC) $(CFLAGS) -c $< -o $@
endif

# --------------------------------------------------------------------
# Tabla de módulos embebidos (con indicador de compresión)
# --------------------------------------------------------------------
$(EMBED_TABLE_SRC): $(FIRE_FILES) $(BIN_FILES)
	@mkdir -p $(dir $@)
	@echo " [GEN] $@"
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
	@echo " [CC] $<"
	$(CC) $(CFLAGS) $(INCDIRS) -c $< -o $@
# --------------------------------------------------------------------
# Reglas para metadatos
# --------------------------------------------------------------------
$(BUILDDIR)/metadata_%.c: $(META_DIR)/%
	@mkdir -p $(dir $@)
	@echo " [META] $<"
	@name=$*; \
	sanename=$$(echo $${name} | tr '-' '_'); \
	echo "unsigned char metadata_$${sanename}[] = {" > $@; \
	od -A n -t x1 -v < "$<" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\{1,\}/, 0x/g' -e 's/^, //' -e 's/^/0x/' -e 's/$$/,/' >> $@; \
	echo "0x00 };" >> $@; \
	echo "unsigned int metadata_$${sanename}_len = sizeof(metadata_$${sanename}) - 1;" >> $@

$(BUILDDIR)/metadata_%.o: $(BUILDDIR)/metadata_%.c
	@echo " [CC] $< (metadata)"
	$(CC) $(CFLAGS) -c $< -o $@

$(META_HUB_SRC): $(patsubst %, $(META_DIR)/%, $(META_FILES))
	@mkdir -p $(dir $@)
	@echo " [GEN] $@"
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
	@echo " [CC] $<"
	$(CC) $(CFLAGS) $(INCDIRS) -c $< -o $@

# Incluir dependencias automáticas si existen
-include $(DEPS)

# --------------------------------------------------------------------
# Objetivo config: crea metadatos si faltan y los abre con el editor
# --------------------------------------------------------------------
config:
	@mkdir -p $(META_DIR)
	@for f in $(META_FILES); do \
		if [ ! -f $(META_DIR)/$$f ]; then \
			echo "Creando $(META_DIR)/$$f con contenido por defecto..."; \
			case $$f in \
				VERSION) echo "1.x" > $(META_DIR)/$$f ;; \
				EDITION) echo "Infernal 1.x (Rama)\n──────────────────────────────────────────────\n\nVersión:           1.x\nEdición:           Para tu app\nArquitectura:      La de tu PC (puedes verlo con uname -m)\nSistema operativo: POSIX (Linux, macOS)\n\nDistribuido por:   Tu Nombre O Apodo\nProyecto:          Tu proyecto\nLicencia:          GPL v3+\n\nCódigo fuente:        https://github.com/tu-user/tu-repo-fork/tree/rama-release\nRepositorio:          https://github.com/tu-user/tu-repo-fork\nRepositorio oficial:  https://github.com/LyndsCorp/Infernal\nDocumentación:        https://github.com/LyndsCorp/Infernal-Documentation\n\nCopyright (C) AÑO Tu empresa/organizacion/nombre" > $(META_DIR)/$$f ;; \
				HELP) echo "Infernal - lenguaje de scripting inspirado en Bash + Lua + Python.\n\nUso:\n  infernal <script.inf> [flags...]\n\nOpciones:\n  --help       Muestra esta ayuda.\n  --version    Muestra la versión del intérprete.\n  --edition    Muestra la edición del intérprete.\n\nSin argumentos muestra el mensaje de bienvenida.\n\nDocumentación: https://github.com/LyndsCorp/Infernal-Documentation" > $(META_DIR)/$$f ;; \
				WELCOME) echo "¡Bienvenido a Infernal!\n\nEscribe infernal --help para recibir ayuda.\n\nInfernal es un lenguaje de programación inspirado en Bash + Lua + Python.\n\nCopyright (C), GPL v3+, Lynds Corp." > $(META_DIR)/$$f ;; \
			esac; \
		fi; \
	done
	@echo "Abriendo metadatos para editar..."
	@if [ -z "$$EDITOR" ]; then EDITOR=nano; fi; \
	$$EDITOR $(META_DIR)/VERSION $(META_DIR)/EDITION $(META_DIR)/HELP $(META_DIR)/WELCOME

# --------------------------------------------------------------------
# Limpieza (respeta config/)
# --------------------------------------------------------------------
clean:
	@echo " [CLEAN]"
	rm -rf $(BUILDDIR) $(TARGET)

distclean: clean
	@echo " [DISTCLEAN]"
	rm -f src/metadata/*

help:
	@echo "Infernal Makefile"
	@echo "-----------------"
	@echo "Objetivos:"
	@echo "             : (vacío; solo ejecuta 'make') hace lo mismo que 'make all'"
	@echo "  all        : compila el intérprete (por defecto)"
	@echo "  debug      : compila con soporte de depuración (-DDEBUG)"
	@echo "  release    : compila optimizado para distribución (-O2, sin debug)"
	@echo "  config     : crea o edita los metadatos (VERSION, HELP, WELCOME, EDITION)"
	@echo "  clean      : elimina objetos y el ejecutable (no toca config/)"
	@echo "  distclean  : elimina todo src/metadata para que hagas make config limpio"
	@echo "  help       : muestra esta ayuda"
	@echo ""
	@echo "Estadísticas:"
	@printf "  Archivos fuente (.c): %d\n" $(words $(SOURCES))
	@printf "  Módulos .fire embebidos: %d\n" $(words $(FIRE_FILES))
	@printf "  Binarios embebidos: %d\n" $(words $(BIN_FILES))
	@printf "  Versión: "
	@cat src/metadata/VERSION || echo "No disponible"

test: $(TARGET)
	@for file in demos/*.inf; do \
		echo " [TEST] $$file"; \
		./$(TARGET) $$file || exit 1; \
	done

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer' \
	        LDFLAGS='$(LDFLAGS) -fsanitize=address,undefined' all
	$(MAKE) test
