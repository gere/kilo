/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE


#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdarg.h>
#include <malloc.h>
#include <fcntl.h>
/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	PAGE_UP,
	PAGE_DOWN,
	HOME_KEY,
	END_KEY,
	DEL_KEY
};

enum editorHiglight {
	HL_NORMAL = 0,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH,  //search match
	HL_COMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** prototypes ***/ 

void editorClearScreen();
void editorSetStatusMessage(const char * format, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** data ***/

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *hl; 	// highlight: each value correspond to a char in render 
					   	// and will tell if the character is part of a string, a comment, a number, ect
} erow;

struct editorConfig {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	int dirty; /* file modified but saved */
	erow *row;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct editorSyntax *syntax;
	struct termios orig_termios;
};

struct editorSyntax {
	char *filetype;
	char **filematch; // array of strings. Each string contains a pattern to match a filename agains.
	char **keywords;

	char *singleline_comment_start;
	int flags;
};

struct editorConfig E;

/*** filetypes ***/
/* NOTE: NULL terminated arrays are great to loop trough them with a while */ 
char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL }; 

// the pipe terminated keywords are the second type of keyword
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL //GREAT NULL terminated array
};

struct editorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//",		
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0])) // length of the hldb array

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}


/*** terminal ***/

void die(const char *s) {	
	editorClearScreen();	
	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}


void enableRawMode() {
	
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);
	struct termios raw = E.orig_termios;

	raw.c_iflag &= ~(BRKINT| ICRNL | IXON | INPCK | ISTRIP);
	raw.c_oflag &= ~(OPOST);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN| ISIG);
	raw.c_cflag |= (CS8);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)	die("tcsetattr");
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch(seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}
			else {
				switch(seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		return '\x1b';		
	} 
	else {	
		return c;
	}
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	
	while (i < sizeof(buf) -1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;		
	}
	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return - 1;
	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	}
	else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** syntax highlighting ***/
int is_separator(int c) {
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
	row->hl = realloc(row->hl, row->rsize);
	memset(row->hl, HL_NORMAL, row->rsize);

	if (E.syntax == NULL) return; // no syntax no party

	char **keywords = E.syntax->keywords;

	char *scs = E.syntax->singleline_comment_start;
	int scs_len = scs ? strlen(scs) : 0;

	int prev_sep = 1; // initialized to true because the beginning of the line is a separator
	int in_string = 0;

	int i = 0;
	while (i < row->rsize) {
		char c = row->render[i];
		unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

		if (scs_len && !in_string) {
			if (!strncmp(&row->render[i], scs, scs_len)) {
				memset(&row->hl[i], HL_COMMENT, row->rsize - i);
				break;
			}
		}

		if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				if (c == '\\' && i + 1 < row->rsize) {
					row->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string) in_string = 0; //in_string is equal to " or ', so, if it's a match, it's a closing quote 
				i++;
				prev_sep = 1;
				continue;
			}
			else {
				if (c == '"' || c == '\'') {
					in_string = c;
					row->hl[i] = HL_STRING;
					i++;
					continue;
				}
			}
		}

		if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
			//TODO: maybe use a regular expression instead of this?
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
				(c == '.' && prev_hl == HL_NUMBER)) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0; 
				continue;
			}
		}

		if (prev_sep) {
			int j = 0;
			char *keyword;
			while ((keyword = keywords[j])) {
				int klen = strlen(keyword);
				int is_kw2 = keyword[klen - 1] == '|';
				if (is_kw2) klen--;

				if (!strncmp(&row->render[i], keyword, klen) &&
					is_separator(row->render[i + klen])) {
					memset(&row->hl[i], is_kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;
					break;
				}

				j++;
			}
		}


		prev_sep = is_separator(c); 
		i++;
	}
}

int editorSyntaxToColor(int hl) {
	switch(hl) {
		case HL_NUMBER: return 196;
		case HL_STRING: return 5;
		case HL_MATCH: return 27;
		case HL_COMMENT: return 6;
		case HL_KEYWORD1: return 11;
		case HL_KEYWORD2: return 10;
		

		default: return 37;
	}
}

void editorSelectSyntaxHighlight() {
	E.syntax = NULL;
	if (E.filename == NULL) return;

	char *ext = strrchr(E.filename, '.');
	for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
		struct editorSyntax *s = &HLDB[j];
		unsigned int i = 0;
		while (s->filematch[i]) {
			int is_ext = (s->filematch[i][0] == '.');
			// check if it's a match with file extension pattern or 
			// with a pattern anywhere in the filename
			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
				(!is_ext && strstr(E.filename, s->filematch[i]))) {
				E.syntax = s;

				int filerow;
				for(filerow = 0; filerow < E.numrows; filerow++) {
					editorUpdateSyntax(&E.row[filerow]);
				}

				return;
			}
			i++;
		}

	}
}

/*** row operations ***/
/*
 * Character index to render index
 */
int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') {
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		}
		rx++;
	}
	return rx;
}

/*
 * Render index to character indes
 */
int editorRowRxToCx(erow *row, int rx) {
	int cur_rx = 0;
	int cx;
	for (cx = 0; cx < row->size; cx++) {
		if (row->chars[cx] == '\t') 
			cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
		cur_rx++;

		if (cur_rx > rx) return cx;
	}
	return cx;
}

void editorUpdateRow(erow *row) {

	int tabs = 0;
	int j = 0;
	for (j=0; j < row->size; j++) {
		if (row->chars[j] == '\t') tabs++;
	}

	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
		}
		else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;

	editorUpdateSyntax(row);
}


void editorInsertRow(int at, char *s, size_t len) {
	if (at < 0 || at > E.numrows) return;

	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
		
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	E.row[at].hl = NULL;

	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow *row) {
	free(row->render);
	free(row->chars);
	free(row->hl);
}

void editorDelRow(int at) {
	if (at < 0 || at >= E.numrows) return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}


void editorRowInsertChar(erow *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;	
	row->chars = realloc(row->chars, row->size + 2); // add two, because the actual length of the buffer (NOT the size value) 
													 //	also includes the null byte to terminate the string			
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1); // memmove must be used insted of memcpy if the memory area overlap
	row->size++;
	row->chars[at] = c;		
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size +len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDeleteChar(erow *row, int at) {
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
	if (E.cy == E.numrows) {
		editorInsertRow(E.numrows, "", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorInsertNewline() {
	if (E.cx == 0) {
		editorInsertRow(E.cy, "", 0);
	}
	else {
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;		
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorDelChar() {
	if (E.cy == E.numrows) return; // last line
	if (E.cx == 0 && E.cy == 0) return; // beginning of the file

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		editorRowDeleteChar(row, E.cx - 1);
		E.cx--;
	}
	else {
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}


/*** file i/o ***/

/**
 * concatenete the rows in a string ready to be saved
 * @param  buflen pointer to int, to return the length of the buffer to caller
 * @return   buffer of chars, ready to be saved
 */
char* editorRowsToString(int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; ++j)	{
		totlen += E.row[j].size + 1; // add one to make room for the new line char
	}
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}
	return buf;
}

void editorOpen(char *filename) {
	E.filename = strdup(filename);

	editorSelectSyntaxHighlight();

	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\r' ||
							  line[linelen - 1] == '\n'))
			linelen--;
		editorInsertRow(E.numrows, line, linelen);
	}
	E.dirty = 0;
	free(line);
	fclose(fp);
}

void editorSave() {
	if (E.filename == NULL) {
		E.filename = editorPrompt("Save as: %s", NULL);

		if (E.filename == NULL) {
			editorSetStatusMessage("Save aborted");
			return;
		}
		editorSelectSyntaxHighlight();		
	}
	
	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/
void editorFindCallback(char *query, int key) {
	static int last_match = -1; // -1 no match, otherwise index of the row of the match
	static int direction = 1; 	//direction of the search: 1 forward, -1 backward

	static int saved_hl_line;
	static char* saved_hl = NULL;

	if (saved_hl) {
		memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl = NULL;
	}

	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	}
	else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	}
	else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	}
	else {
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1) direction = 1;
	int current = last_match; //index of the row we are curently searcing
	int i;
	for (i = 0; i <E.numrows; i++) {
		current += direction;
		if (current == -1) current = E.numrows - 1;  // wrap back
		else if (current == E.numrows)  current = 0; // wrap forward

		erow *row = &E.row[current];
		char *match = strstr(row->render, query);

		if (match) {
			last_match = current;
			E.cy = current; 
			E.cx = editorRowRxToCx(row, match - row->render);
			E.rowoff = E.numrows;

			saved_hl_line = current;
			saved_hl = malloc(row->rsize);
			memcpy(saved_hl, row->hl, row->rsize);
			memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void editorFind() {
	int saved_cx = E.cx;
	int saved_cy = E.cy;
	int saved_coloff = E.coloff;
	int saved_rowoff = E.rowoff;



	char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
	
	if (query)	{
		free(query);
	}
	else {
		E.cx = saved_cx;
		E.cy = saved_cy;
		E.coloff = saved_coloff;
		E.rowoff = saved_rowoff;
	}
}

/*** input ***/

char *editorPrompt(char *prompt, void(*callback)(char *, int)) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while(1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();

		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			editorSetStatusMessage("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editorSetStatusMessage("");
				if (callback) callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) { //check it's not a special key
			if (buflen == bufsize -1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
		if (callback) callback(buf, c);
	}
}

erow *getCurrentRow() {
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy]; 
	return row;
}

void editorMoveCursor(int key) {
	erow *row = getCurrentRow();
	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0)	{
				E.cx--;
			}
			else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) { 
				E.cx++;
			} else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows) {
				E.cy++;
			}			
			break;			
	}
	row = getCurrentRow();
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) E.cx = rowlen;
}

void editorProcessKeypress() {
	static int quit_times = KILO_QUIT_TIMES;

	int c = editorReadKey();
	switch (c) {
		case '\r': 
			editorInsertNewline();
			break;
		case CTRL_KEY('q'):	{		
			if (E.dirty && quit_times > 0) {
				editorSetStatusMessage("Warning! File has unsaved changes. " 
					"Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}	
			editorClearScreen();
			exit(0);
			}
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;	
		case PAGE_UP:			
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					E.cy = E.rowoff;
				}
				else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows - 1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				}
				int times = E.screenrows;
				while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;
		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			if (E.cy < E.numrows) {
				E.cx = E.row[E.cy].size;	
			}			
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			//TODO
			break;

		case CTRL_KEY('s'):
			editorSave();
			break;

		case CTRL_KEY('f'):
			editorFind();
			break;

		default:
			editorInsertChar(c);
			break;		
	}
	quit_times = KILO_QUIT_TIMES;
}

/*** output ***/

void editorScroll() {
	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}	
	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}

	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}

	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; ++y){
		int filerow = y + E.rowoff;		
		// if we are after the end of the file
		if (filerow >= E.numrows) {
			// if there are no rows
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
					"Kilo editor -- version %s", KILO_VERSION);
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "~", 1);
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen); 
			} else {
				abAppend(ab, "~", 1);	
			}
		}
		else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;

			
			char *c  =  &E.row[filerow].render[E.coloff];
			unsigned char *hl =  &E.row[filerow].hl[E.coloff];
			int current_color = HL_NORMAL;
			int j;
			for (j = 0; j < len; j++) {
				if (hl[j] == HL_NORMAL) {
					if (current_color != HL_NORMAL) {
						abAppend(ab, "\x1b[39m", 5);
						current_color = HL_NORMAL;
					}
					abAppend(ab, &c[j], 1);
				} else {
					int color = editorSyntaxToColor(hl[j]);
					if (current_color != color) {
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", color);
						abAppend(ab, buf, clen); //https://en.wikipedia.org/wiki/ANSI_escape_code#Colors						
						current_color = color;
					}	
					abAppend(ab, &c[j], 1);				
				}
			}
			abAppend(ab, "\x1b[39m", 5);
			
		}

		abAppend(ab, "\x1b[K", 3); //Erase in Line http://vt100.net/docs/vt100-ug/chapter3.html#EL
		
		abAppend(ab, "\r\n", 2);
		
	}
}

void editorDrawStatusBar(struct abuf *ab) {
	abAppend(ab, "\x1b[7m", 4); //invert colors
	
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
		E.filename ? E.filename : "[No name]", E.numrows,
		E.dirty > 0 ? "(modified)" : "");


	int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
						E.syntax ? E.syntax->filetype : "no ft", 
						E.cy + 1, 
						E.numrows);

	abAppend(ab, status, len);

	while (len < E.screencols) {
		if (E.screencols - len == rlen) { // right align the current row number
			abAppend(ab, rstatus, rlen);
			break;
		}
		abAppend(ab, " ", 1);
		len++;		
	}
	abAppend(ab, "\x1b[m", 3); //bring normal colors back
	abAppend(ab, "\r\n", 2); // make room for another line
}

void editorDrawMessageBar(struct abuf *ab) {
	abAppend(ab, "\x1b[K", 3); //clear the line
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5) {
		abAppend(ab, E.statusmsg, msglen);
	}
}



void editorRefreshScreen() {	

	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6); //hide the cursors
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, 
											  (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	
	abAppend(&ab, "\x1b[?25h", 6); //show the cursor

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char * format, ...) {
	va_list ap;
	va_start(ap, format);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), format, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}


void editorClearScreen() {
	
	write(STDOUT_FILENO, "\x1b[2J", 4); //erase in display http://vt100.net/docs/vt100-ug/chapter3.html#ED
	write(STDOUT_FILENO, "\x1b[H", 3); //reposition cursor to top first row, first col: http://vt100.net/docs/vt100-ug/chapter3.html#CUP
}

/*** init ***/

void initEditor() {
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = 0;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.syntax = NULL;


	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2; // reduce the number of shown rows to add space for status bar
	

}

int main(int argc, char *argv[]) {
	
	enableRawMode();
	initEditor();
	if (argc >= 2)
		editorOpen(argv[1]);
	
	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-F = find | Ctrl-Q = quit");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
		/*char c = '\0';
		
		if (iscntrl(c)) {
			printf("%d\r\n", c);
		}
		else {
			printf("%d ('%c')\r\n", c, c);
		}
		if (c == CTRL_KEY('q')) break;*/
		
	}
	return 0;
}



