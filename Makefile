CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c17
SRC = bloc.c
OUT = bloc

ifeq ($(OS),Windows_NT)
    RM = del
    EXE = .exe
else
    RM = rm -f
    EXE =
endif

all: $(OUT)$(EXE)

$(OUT)$(EXE): $(SRC)
	@echo "Compiling $(SRC) to $(OUT)$(EXE)..."
	$(CC) $(CFLAGS) $(OS_FLAGS) $(SRC) -o $(OUT)$(EXE)

clean:
	$(RM) $(OUT)$(EXE)