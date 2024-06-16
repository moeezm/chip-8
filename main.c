#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// ==== MACROS ====
// ambiguous instruction options
#define SHIFT_USES_Y true
#define CHANGE_I true 

#define DEBUG false 

#define min(x, y) (((x) < (y)) ? x : y)
#define max(x, y) (((x) > (y)) ? x : y)

#define PIXEL_SIZE 16 
#define GRID_THICK 2
#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 32
#define _convertX(x) (((GRID_THICK) + (PIXEL_SIZE))*(x) + (GRID_THICK))
#define _convertY(y) (((GRID_THICK) + (PIXEL_SIZE))*(y) + (GRID_THICK))

#define MEMORY_SIZE 4096
#define MAX_STACK 256
#define N_REGISTERS 16

#define FONT_START 0x50

#define MAX_KEYPAD_KEYCODE 33

// how many ops per second
#define CLOCK_SPEED 100
// how often delay timer and sound timer should be decremented (also in Hz)
#define TIMER_SPEED 60

#define _join2(a, b) (((a)<<4)+(b))
#define _join3(a, b, c) (((a)<<8)+((b)<<4)+(c))
#define _nibble1(a) (((a)&(((1<<4)-1)<<4))>>4)
#define _nibble2(a) ((a)&((1<<4)-1))

Uint8 font[] = {0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
0x20, 0x60, 0x20, 0x20, 0x70, // 1
0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
0x90, 0x90, 0xF0, 0x10, 0x10, // 4
0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
0xF0, 0x10, 0x20, 0x40, 0x40, // 7
0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
0xF0, 0x90, 0xF0, 0x90, 0x90, // A
0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
0xF0, 0x80, 0x80, 0x80, 0xF0, // C
0xE0, 0x90, 0x90, 0x90, 0xE0, // D
0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};

// keypad is going to be 1234QWERASDFZXCV
Uint8 keypad[MAX_KEYPAD_KEYCODE+1];

const int WINDOW_WIDTH = _convertX(SCREEN_WIDTH);
const int WINDOW_HEIGHT = _convertY(SCREEN_HEIGHT);

Uint32* pixels; // actually displayed screen
SDL_Window* window;
SDL_Surface* surface;
Uint32 grid[SCREEN_HEIGHT][SCREEN_WIDTH];

void setPixel(int x, int y, Uint32 color) {
	// color is just 0 or 1
	grid[y][x] = color;
	if (color == 1) color = 0xFFFFFFFF;
	int start = _convertY(y)*WINDOW_WIDTH + _convertX(x);
	for (int i = 0; i < PIXEL_SIZE; i++) {
		for (int j = 0; j < PIXEL_SIZE; j++) {
			pixels[start + i*WINDOW_WIDTH + j] = color;
		}
	}
}

void initWindow() {
	printf("%d x %d\n", WINDOW_WIDTH, WINDOW_HEIGHT);
	window = NULL;
	surface = NULL;
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("SDL could not be initialized: %s", SDL_GetError());
		exit(1);
	}
	window = SDL_CreateWindow(
			"SDL Basic",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			WINDOW_WIDTH,
			WINDOW_HEIGHT,
			SDL_WINDOW_SHOWN
	);
	if (window == NULL) {
		printf("Window could not be created: %s", SDL_GetError());
		exit(1);
	}
	surface = SDL_GetWindowSurface(window);
	pixels = surface->pixels;
}

void cleanWindow() {
	SDL_DestroyWindow(window);
	SDL_Quit();
}

Uint8 mem[MEMORY_SIZE];
Uint16 pc = 0x200; // program counter, first 512 bytes "reserved"
Uint16 I = 0; // index register
Uint8 regs[N_REGISTERS]; // gen-purpose registers
Uint16 stack[MAX_STACK];
Uint8 sp = 0; // stack pointer

void initMem() {
	memset(mem, 0, MEMORY_SIZE);
	memset(regs, 0, N_REGISTERS);
	memset(grid, 0, SCREEN_WIDTH * SCREEN_HEIGHT);
}

void initFont() {
	memcpy(mem, font, 50);
}

void initKeypad() {
	// 1 2 3 4
	// Q W E R
	// A S D F
	// Z X C V
	// maps to
	// 1 2 3 C
	// 4 5 6 D
	// 7 8 9 E
	// A 0 B F
	keypad[30] = 1;
	keypad[31] = 2;
	keypad[32] = 3;
	keypad[33] = 0xC;
	keypad[20] = 4;
	keypad[26] = 5;
	keypad[8] = 6;
	keypad[21] = 0xD;
	keypad[4] = 7;
	keypad[22] = 8;
	keypad[7] = 9;
	keypad[9] = 0xE;
	keypad[29] = 0xA;
	keypad[27] = 0;
	keypad[6] = 0xB;
	keypad[25] = 0xF;
}

volatile int delayTimer = 0;
volatile int soundTimer = 0;
volatile bool timerRunning = true;
pthread_t timerThread;
pthread_mutex_t timerLock;

void* timerLoop(void* arg) {
	while (timerRunning) {
		pthread_mutex_lock(&timerLock);
		if (delayTimer > 0) delayTimer--;
		if (soundTimer > 0) soundTimer--;
		pthread_mutex_unlock(&timerLock);
		usleep(1000000 / TIMER_SPEED);
	}
}

void startTimers() {
	pthread_mutex_init(&timerLock, NULL);
	pthread_create(&timerThread, NULL, timerLoop, NULL);
}

void stopTimers() {
	timerRunning = false;
	pthread_join(timerThread, NULL);
	pthread_mutex_destroy(&timerLock);
}

void loadProgram(char* filename) {
	FILE* file = fopen(filename, "r");
	if (file == NULL) {
		printf("Failed to open file: %s\n", filename);
		exit(1);
	}
	size_t read_count = fread(mem+pc, sizeof(Uint8), MEMORY_SIZE-pc, file);
	printf("Read: %ld\n", read_count);
}

int main() {
	srand(time(NULL));
	
	initKeypad();
	initMem();
	initFont();
	char romsource[] = "roms/octoachip8story.ch8";
	loadProgram(romsource);
	startTimers();
	initWindow();

	SDL_Event e;

	printf("bool size: %ld\n", sizeof(bool));
	bool held[16];
	bool pressed[16];
	for (int i = 0; i < 16; i++) held[i] = false, pressed[i] = false;
	int running = 1;
	while (running) {
		bool anyKeyPressed = false;
		Uint8 lastKey = 0;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
				case SDL_QUIT:
					running = 0;
					break;
				case SDL_KEYDOWN:
					held[keypad[e.key.keysym.scancode]] = true;
					pressed[keypad[e.key.keysym.scancode]] = true;
					lastKey = keypad[e.key.keysym.scancode];
					anyKeyPressed = true;
					break;
				case SDL_KEYUP:
					held[keypad[e.key.keysym.scancode]] = false;
					break;
			}
		}
		Uint16 code, regX, regY, N, NN, NNN;
		code = _nibble1(mem[pc]);
		regX = _nibble2(mem[pc]);
		regY = _nibble1(mem[pc+1]);
		N = _nibble2(mem[pc+1]);
		NN = mem[pc+1];
		NNN = (regX<<8) + NN;

		if (DEBUG) printf("PC, byte 1, byte 2: %X, %X, %X\n", pc, mem[pc], mem[pc+1]);

		pc += 2;
		// unsigned and signed answer variables of larger size
		unsigned int uans;
		int sans;
		switch (code) {
			case 0:
				if (NNN == 0x0E0) {
					// 0x00E0
					// clear screen
					for (int x = 0; x < SCREEN_WIDTH; x++) {
						for (int y = 0; y < SCREEN_HEIGHT; y++) {
							setPixel(x, y, 0);
						}
					}
					SDL_UpdateWindowSurface(window);
				}
				else if (NNN == 0x0EE) {
					// 0x0EE
					// return from subroutine
					pc = stack[sp--];
				}
				break;
			case 1:
				// 0x1NNN
				// jump/set program counter
				pc = NNN;
				break;
			case 2:
				stack[++sp] = pc;
				pc = NNN;
				break;
			case 3:
				// 0x3XNN
				// skip next instruction if registerX = NN
				if (regs[regX] == NN) {
					pc+=2;
				}
				break;
			case 4:
				// 0x4XNN
				// skip next instruction if registerX != NN
				if (regs[regX] != NN) {
					pc += 2;
				}
				break;
			case 5:
				// 0x5XY0
				// skip if registerX == registerY
				if (regs[regX] == regs[regY]) {
					pc += 2;
				}
				break;
			case 6:
				// 0x6XNN
				// set register
				regs[regX] = NN;
				break;
			case 7:
				// 0x7XNN
				// add to register
				regs[regX] += NN;
				break;
			case 8:
				// 0x8XYN
				// various logical/arithmetic operations
				switch (N) {
					case 0:
						regs[regX] = regs[regY];
						break;
					case 1:
						regs[regX] |= regs[regY];
						break;
					case 2:
						regs[regX] &= regs[regY];
						break;
					case 3:
						regs[regX] ^= regs[regY];
						break;
					case 4:
						// unsoulful way to do it
						uans = regs[regX] + regs[regY];
						regs[regX] = uans%256;
						regs[0xF] = (uans > 255);
						break;
					case 5:
						// even more unsoulful than 4
						sans = regs[regX] - regs[regY];
						regs[regX] = (sans+256)%256;
						regs[0xF] = (sans >= 0);
						break;
					case 6:
						if (SHIFT_USES_Y) regs[regX] = regs[regY];
						regs[0xF] = regs[regX]&1;
						regs[regX] >>= 1;
						break;
					case 7:
						sans = regs[regY] - regs[regX];
						regs[regX] = (sans + 256)%256;
						regs[0xF] = (sans >= 0);
						break;
					case 0xE:
						if (SHIFT_USES_Y) regs[regX] = regs[regY];
						regs[0xF] = (regs[regX]>>7)&1;
						regs[regX] <<= 1;
						break;
				}
				break;
			case 9:
				// 0x9XY0
				// skip if register X != register Y
				if (regs[regX] != regs[regY]) {
					pc += 2;
				}
				break;
			case 0xA:
				// 0xANNN
				// set index register
				I = NNN;
				break;
			case 0xB:
				pc = NNN + regs[0];
				break;
			case 0xC:
				// 0xCXNN
				// sets register X to rand value AND with NN
				regs[regX] = (Uint8)(rand()%256) & NN;
				break;
			case 0xD:
				// 0xDXYN
				// draw sprite
				Uint8 x = regs[regX] % SCREEN_WIDTH;
				Uint8 y = regs[regY] % SCREEN_HEIGHT;
				regs[0xF] = 0;
				for (Uint8 j = 0; j < min(SCREEN_HEIGHT - y, N); j++) {
					for (Uint8 i = 0; i < min(SCREEN_WIDTH - x, 8); i++) {
						if ((mem[I+j]>>(7-i))&1) {
							if (grid[y+j][x+i]) regs[0xF] = 1;
							setPixel(x+i, y+j, 1-grid[y+j][x+i]);
						}
					}
				}
				SDL_UpdateWindowSurface(window);
				break;
			case 0xE:
				switch (NN) {
					case 0x9E:
						if (held[regs[regX]]) pc += 2;
						break;
					case 0xA1:
						if (!held[regs[regX]]) pc += 2;
						break;
				}
				break;
			case 0xF:
				// misc instructions
				switch (NN) {
					case 0x07:
						pthread_mutex_lock(&timerLock);
						regs[regX] = delayTimer;
						pthread_mutex_unlock(&timerLock);
						break;
					case 0x0A:
						if (!anyKeyPressed) pc -= 2;
						else regs[regX] = lastKey;
					case 0x15:
						pthread_mutex_lock(&timerLock);
						delayTimer = regs[regX];
						pthread_mutex_unlock(&timerLock);
						break;
					case 0x18:
						pthread_mutex_lock(&timerLock);
						soundTimer = regs[regX];
						pthread_mutex_unlock(&timerLock);
						break;
					case 0x1E:
						uans = I + regs[regX];
						I = uans&((1<<16)-1); // mod
						regs[0xF] = (uans >= (1<<16));
						break;
					case 0x29:
						I = FONT_START + 5*_nibble1(regs[regX]);
						break;
					case 0x33:
						Uint8 n_digits = 0;
						Uint8 tmp = regs[regX];
						while (tmp > 0) {
							mem[I + n_digits] = (tmp%10);
							n_digits++;
							tmp /= 10;
						}
						// reverse digits in memory
						for (int i = 0; i < n_digits/2; i++) {
							tmp = mem[I + i];
							mem[I + i] = mem[I + n_digits - i - 1];
							mem[I + n_digits - i - 1] = tmp;
						}
						break;
					case 0x55:
						for (int i = 0; i <= regX; i++) {
							mem[I+i] = regs[i];
						}
						if (CHANGE_I) I += (regX + 1);
						break;
					case 0x65:
						for (int i = 0; i <= regX; i++) {
							regs[i] = mem[I+i];
						}
						if (CHANGE_I) I += (regX + 1);
						break;
				}
				break;
		}
		usleep(1000000 / CLOCK_SPEED);
	}
	stopTimers();
	cleanWindow();
	return 0;
}
