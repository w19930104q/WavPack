////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// utils.c

// This module provides general purpose utilities for the WavPack command-line
// utilities and the self-extraction module.

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500 /* for GetConsoleWindow() */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <conio.h>
#include <shlobj.h>
#elif defined(__GNUC__) || defined(__sun)
#include <glob.h>
#include <unistd.h>
#endif

#include <sys/stat.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "wavpack.h"
#include "utils.h"

#ifdef _WIN32
#include "win32_unicode_support.h"
#define fprintf fprintf_utf8
#define fputs fputs_utf8
#define remove(f) unlink_utf8(f)
#endif

#ifdef __MINGW32__              // mingw32's _ftelli64() and _fseeki64() are defective
#define _ftelli64 ftello64
#define _fseeki64 fseeko64
#endif

// The C-standard function strtod() also handles hex numbers prefixed
// with [+-]0[xX]. Unfortunately this causes problems for us in rare
// cases where a value of zero is specified for one option followed
// by the 'x' option (e.g., -s0x1). This version of strtod() does not
// allow hex specification, but otherwise should be identical.

double strtod_hexfree (const char *nptr, char **endptr)
{
    const char *sptr = nptr;

    // skip past any leading whitespace and possibly a sign
    while (isspace ((unsigned char)*sptr)) sptr++;
    if (*sptr == '+' || *sptr == '-') sptr++;

    // if hex detected ("0x" or "0X"), return 0.0 and end at the X
    if (*sptr == '0' && tolower ((unsigned char)sptr[1]) == 'x') {
        if (endptr) *endptr = (char *) sptr + 1;
        return 0.0;
    }

    // otherwise unmodified strtod() result
    return strtod (nptr, endptr);
}

#ifdef _WIN32

int copy_timestamp (const char *src_filename, const char *dst_filename)
{
    wchar_t *src_filename_utf16 = utf8_to_utf16(src_filename);
    wchar_t *dst_filename_utf16 = utf8_to_utf16(dst_filename);
    FILETIME last_modified;
    HANDLE src, dst;
    int res = TRUE;

    if (*src_filename == '-' || *dst_filename == '-')
        return res;

    if (!src_filename_utf16 || !dst_filename_utf16)
        return FALSE;

    src = CreateFileW (src_filename_utf16, GENERIC_READ, FILE_SHARE_READ, NULL,
         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    dst = CreateFileW (dst_filename_utf16, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    if (src == INVALID_HANDLE_VALUE || dst == INVALID_HANDLE_VALUE ||
        !GetFileTime (src, NULL, NULL, &last_modified) ||
        !SetFileTime (dst, NULL, NULL, &last_modified))
            res = FALSE;

    if (src != INVALID_HANDLE_VALUE)
        CloseHandle (src);

    if (dst != INVALID_HANDLE_VALUE)
        CloseHandle (dst);

    free (src_filename_utf16);
    free (dst_filename_utf16);

    return res;
}

#else

#include <sys/time.h>
#include <sys/types.h>

int copy_timestamp(const char *src_filename, const char *dst_filename)
{
    struct stat fileinfo;
    struct timeval times[2];

    if (strcmp(src_filename, "-") == 0 || strcmp(dst_filename, "-") == 0)
        return TRUE;

    if (stat(src_filename, &fileinfo))
        return FALSE; /* stat failed */

    times[0].tv_sec  = fileinfo.st_atime;
    times[0].tv_usec = 0;

    times[1].tv_sec  = fileinfo.st_mtime;
    times[1].tv_usec = 0;

    if (utimes(dst_filename, times))
        return FALSE; /* utimes failed */

    return TRUE;
}

#endif

//////////////////////////////////////////////////////////////////////////////
// This function parses a filename (with or without full path) and returns  //
// a pointer to the extension (including the "."). If no extension is found //
// then NULL is returned. Extensions with more than 4 letters don't count.  //
//////////////////////////////////////////////////////////////////////////////

#if defined(_WIN32)

char *filespec_ext (char *filespec)
{
    char *cp = filespec + strlen (filespec);

    while (--cp >= filespec) {

        if (*cp == '\\' || *cp == ':')
            return NULL;

        if (*cp == '.') {
            if (strlen (cp+1) && strlen (cp+1) <= 4)
                return cp;
            else
                return NULL;
        }
    }

    return NULL;
}

#else

char *filespec_ext (char *filespec)
{
    char *cp = filespec + strlen (filespec);

    while (--cp >= filespec) {

        if (*cp == '/')
            return NULL;

        if (*cp == '.') {
            if (strlen (cp+1) && strlen (cp+1) <= 4)
                return cp;
            else
                return NULL;
        }
    }

    return NULL;
}

#endif

//////////////////////////////////////////////////////////////////////////////
// This function determines if the specified filespec is a valid pathname.  //
// If not, NULL is returned. If it is in the format of a pathname, then the //
// original pointer is returned. If the format is ambiguous, then a lookup  //
// is performed to determine if it is in fact a valid path, and if so a "\" //
// is appended so that the pathname can be used and the original pointer is //
// returned.                                                                //
//////////////////////////////////////////////////////////////////////////////

#if (defined(__GNUC__) || defined(__sun)) && !defined(_WIN32)

char *filespec_path (char *filespec)
{
    char *cp = filespec + strlen (filespec);
    glob_t globs;
    struct stat fstats;

    if (cp == filespec || filespec_wild (filespec))
        return NULL;

    if (*--cp == '/')
        return filespec;

    if (*cp == '.' && cp == filespec)
        return strcat (filespec, "/");

    if (glob (filespec, GLOB_MARK|GLOB_NOSORT, NULL, &globs) == 0 &&
        globs.gl_pathc > 0)
    {
        /* test if the file is a directory */
        if (stat(globs.gl_pathv[0], &fstats) == 0 && (fstats.st_mode & S_IFDIR)) {
                filespec[0] = '\0';
                strcat (filespec, globs.gl_pathv[0]);
                globfree(&globs);
                return filespec;
        }
    }
    globfree(&globs);

    return NULL;
}

#else

char *filespec_path (char *filespec)
{
    char *cp = filespec + strlen (filespec);
    struct _wfinddata_t wfinddata;
    wchar_t *filespec_utf16;
    intptr_t file;

    if (cp == filespec || filespec_wild (filespec))
        return NULL;

    --cp;

    if (*cp == '\\' || *cp == ':')
        return filespec;

    if (*cp == '.' && cp == filespec)
        return strcat (filespec, "\\");

    filespec_utf16 = utf8_to_utf16(filespec);

    if (!filespec_utf16)
        return NULL;

    if ((file = _wfindfirst (filespec_utf16, &wfinddata)) != (intptr_t) -1 &&
        (wfinddata.attrib & _A_SUBDIR)) {
            _findclose (file);
            free (filespec_utf16);
            return strcat (filespec, "\\");
    }

    if (file != -1L)
        _findclose(file);

    free (filespec_utf16);
    return NULL;
}

#endif

//////////////////////////////////////////////////////////////////////////////
// This function returns non-NULL if the specified filename spec has any    //
// wildcard characters.                                                     //
//////////////////////////////////////////////////////////////////////////////

char *filespec_wild (char *filespec)
{
    return strpbrk (filespec, "*?");
}

//////////////////////////////////////////////////////////////////////////////
// This function parses a filename (with or without full path) and returns  //
// a pointer to the actual filename, or NULL if no filename can be found.   //
//////////////////////////////////////////////////////////////////////////////

#if defined(_WIN32)

char *filespec_name (char *filespec)
{
    char *cp = filespec + strlen (filespec);

    while (--cp >= filespec) {

        if (*cp == '\\' || *cp == ':')
            break;
    }

    if (strlen (cp + 1))
        return cp + 1;
    else
        return NULL;
}

#else

char *filespec_name (char *filespec)
{
    char *cp = filespec + strlen (filespec);

    while (--cp >= filespec)
        if (*cp == '/')
            break;

    if (strlen (cp + 1))
        return cp + 1;
    else
        return NULL;
}

#endif

//////////////////////////////////////////////////////////////////////////////
// This function allows the user to type 'y', 'n', or 'a' (with Enter) in   //
// response to a system query. The return value is the key typed as         //
// lowercase (regardless of the typed case).                                //
//////////////////////////////////////////////////////////////////////////////

static int waiting_input;

char yna (void)
{
    char choice = 0;
    int key;

    waiting_input = 1;

    while (1) {
#if defined(_WIN32)
        key = _getch ();
#else
        key = fgetc(stdin);
#endif
        if (key == 3) {
            fprintf (stderr, "^C\n");
            exit (1);
        }
        else if (key == EOF) {
            fprintf (stderr, "\r\n");
            exit (1);
        }
        else if (key == '\r' || key == '\n') {
            if (choice) {
                fprintf (stderr, "\r\n");
                fflush (stderr);
                break;
            }
            else {
                fprintf (stderr, "%c", 7);
                fflush (stderr);
            }
        }
        else if (key == 'Y' || key == 'y') {
#ifdef _WIN32
            fprintf (stderr, "%c\b", key);
            fflush (stderr);
#endif
            choice = 'y';
        }
        else if (key == 'N' || key == 'n') {
#ifdef _WIN32
            fprintf (stderr, "%c\b", key);
            fflush (stderr);
#endif
            choice = 'n';
        }
        else if (key == 'A' || key == 'a') {
#ifdef _WIN32
            fprintf (stderr, "%c\b", key);
            fflush (stderr);
#endif
            choice = 'a';
        }
        else {
            fprintf (stderr, "%c", 7);
            fflush (stderr);
        }
    }

    waiting_input = 0;

    return choice;
}

//////////////////////////////////////////////////////////////////////////////
// Display the specified message on the console through stderr. Note that   //
// the cursor may start anywhere in the line and all text already on the    //
// line is erased. A terminating newline is not needed and function works   //
// with printf strings and args.                                            //
//////////////////////////////////////////////////////////////////////////////

extern int debug_logging_mode;

#ifdef _WIN32

typedef HRESULT (WINAPI *getfolderpath_t)(HWND,int,HANDLE,DWORD,LPSTR); /* SHGetFolderPathA */
typedef BOOL (WINAPI *getspecialfolderpath_t)(HWND,LPSTR,int,BOOL);     /* SHGetSpecialFolderPathA */

int get_app_path (char *app_path)
{
    static char file_path [MAX_PATH], tried, result;

    HINSTANCE hinstLib;
    getfolderpath_t getfolderpath;

    if (tried) {
        if (result)
            strcpy (app_path, file_path);

        return result;
    }

    tried = TRUE;
    hinstLib = LoadLibrary ("shell32.dll");

    if (hinstLib) {
        getfolderpath = (getfolderpath_t) GetProcAddress (hinstLib, "SHGetFolderPathA");

        if (getfolderpath && SUCCEEDED (getfolderpath (NULL, CSIDL_APPDATA | 0x8000, NULL, 0, file_path)))
            result = TRUE;

        if (!result) {
            getspecialfolderpath_t getspecialfolderpath;
            getspecialfolderpath = (getspecialfolderpath_t) GetProcAddress (hinstLib, "SHGetSpecialFolderPathA");
            if (getspecialfolderpath && getspecialfolderpath (NULL, file_path, CSIDL_APPDATA, TRUE) != 0)
                result = TRUE;
        }

        FreeLibrary (hinstLib);
    }

    if (!result) {
        hinstLib = LoadLibrary ("shfolder.dll");

        if (hinstLib) {
            getfolderpath = (getfolderpath_t) GetProcAddress (hinstLib, "SHGetFolderPathA");

            if (getfolderpath && SUCCEEDED (getfolderpath (NULL, CSIDL_APPDATA | 0x8000, NULL, 0, file_path)))
                result = TRUE;

            FreeLibrary (hinstLib);
        }
    }

    if (result)
        strcpy (app_path, file_path);

    return result;
}

void do_pause_mode (void)
{
    HWND consoleWnd = GetConsoleWindow ();

    if (!consoleWnd)                    // if there's no console window, don't pause
        return;

    if (!IsWindowVisible(consoleWnd))   // if the console is not visble, don't pause
        return;

    fprintf (stderr, "\n");
    SetConsoleTitle ("Paused");
    system ("pause");
    fprintf (stderr, "\n");
}

void error_line (char *error, ...)
{
    char error_msg [2048];
    va_list argptr;

    error_msg [0] = '\r';
    va_start (argptr, error);
    vsnprintf (error_msg + 1, sizeof (error_msg) - 1, error, argptr);
    va_end (argptr);
    fputs (error_msg, stderr);
    finish_line ();

    if (debug_logging_mode) {
        char file_path [MAX_PATH];
        FILE *error_log = NULL;

        if (get_app_path (file_path)) {
            strcat (file_path, "\\WavPack\\wavpack.log");
            error_log = fopen (file_path, "a+");

            if (!error_log) {
                get_app_path (file_path);
                strcat (file_path, "\\WavPack");

                if (CreateDirectory (file_path, NULL)) {
                    strcat (file_path, "\\wavpack.log");
                    error_log = fopen (file_path, "a+");
                }
            }
        }

        if (!error_log)
            error_log = fopen ("c:\\wavpack.log", "a+");

        if (error_log) {
            fputs (error_msg + 1, error_log);
            fputc ('\n', error_log);
            fclose (error_log);
        }
    }
}

#else

void error_line (char *error, ...)
{
    char error_msg [2048];
    va_list argptr;

    error_msg [0] = '\r';
    va_start (argptr, error);
    vsnprintf (error_msg + 1, sizeof (error_msg) - 1, error, argptr);
    va_end (argptr);
    fputs (error_msg, stderr);
    finish_line ();
}

#endif

//////////////////////////////////////////////////////////////////////////////
// Function to intercept ^C or ^Break typed at the console.                 //
//////////////////////////////////////////////////////////////////////////////
#if defined(_WIN32)
static int break_flag;

BOOL WINAPI ctrl_handler (DWORD ctrl)
{
    if (ctrl == CTRL_C_EVENT) {
        break_flag = TRUE;
        return TRUE;
    }

    if (ctrl == CTRL_BREAK_EVENT) {

        if (waiting_input) {
            return FALSE;
        }
        else {
            break_flag = TRUE;
            return TRUE;
        }
    }

    return FALSE;
}

//////////////////////////////////////////////////////////////////////////////
// Function to initialize console for intercepting ^C and ^Break.           //
//////////////////////////////////////////////////////////////////////////////

void setup_break (void)
{
    HANDLE hConIn = GetStdHandle (STD_INPUT_HANDLE);

    SetConsoleMode (hConIn, ENABLE_PROCESSED_INPUT);
    FlushConsoleInputBuffer (hConIn);
    SetConsoleCtrlHandler (ctrl_handler, TRUE);
    break_flag = 0;
}

//////////////////////////////////////////////////////////////////////////////
// Function to determine whether ^C or ^Break has been issued by user.      //
//////////////////////////////////////////////////////////////////////////////

int check_break (void)
{
    return break_flag;
}

//////////////////////////////////////////////////////////////////////////////
// Function to clear the stderr console to the end of the current line (and //
// go to the beginning next line).                                          //
//////////////////////////////////////////////////////////////////////////////

void finish_line (void)
{
    HANDLE hConIn = GetStdHandle (STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO coninfo;

    if (hConIn && GetConsoleScreenBufferInfo (hConIn, &coninfo) &&
        (coninfo.dwCursorPosition.X || coninfo.dwCursorPosition.Y)) {
            DWORD spaces = coninfo.dwSize.X - coninfo.dwCursorPosition.X, written;
            COORD cpos;
            cpos.X = coninfo.dwCursorPosition.X;
            cpos.Y = coninfo.dwCursorPosition.Y;
            FillConsoleOutputCharacter (hConIn, ' ', spaces, cpos, &written);
            fprintf (stderr, "\n");
    }
    else
        fprintf (stderr, "                                \n");

    fflush (stderr);
}
#else
//////////////////////////////////////////////////////////////////////////////
// Function to clear the stderr console to the end of the current line (and //
// go to the beginning next line).                                          //
//////////////////////////////////////////////////////////////////////////////

void finish_line (void)
{
    fprintf (stderr, "                                \n");
    fflush (stderr);
}

//////////////////////////////////////////////////////////////////////////////
// Function to initialize console for intercepting ^C and ^Break.           //
//////////////////////////////////////////////////////////////////////////////

#include <signal.h>

static int break_flag;

static void int_handler(int s)
{
    break_flag = 1;
}

void setup_break (void)
{
    struct sigaction sigIntHandler;

    break_flag = 0;
    sigIntHandler.sa_handler = int_handler;
    sigemptyset (&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;
    sigaction (SIGINT, &sigIntHandler, NULL);
}

//////////////////////////////////////////////////////////////////////////////
// Function to determine whether ^C or ^Break has been issued by user.      //
//////////////////////////////////////////////////////////////////////////////

int check_break (void)
{
    return break_flag;
}

#endif

///////////////////////////////////////////////////////////////////////////
// Determine the number of processor cores available for multi-threading //
// This number, up to four, will be the default number of worker threads //
///////////////////////////////////////////////////////////////////////////

#ifdef ENABLE_THREADS

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/sysctl.h>
#elif defined(__GNUC__) && !defined(_WIN32)
#include <sys/sysinfo.h>
#endif

int get_default_worker_threads (void)
{
    int num_processors = 1;

#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo (&sysinfo);
    num_processors = sysinfo.dwNumberOfProcessors;
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    size_t len = sizeof (num_processors);
    int mib[2] = { CTL_HW, HW_NCPU };
    sysctl (mib, 2, &num_processors, &len, NULL, 0);
#elif defined (__GNUC__)
    num_processors = get_nprocs ();
#endif

    if (num_processors <= 1)
        return 0;
    else if (num_processors > 4)
        return 4;
    else
        return num_processors;
}

#endif

//////////////////////////// File I/O Wrapper ////////////////////////////////

int DoReadFile (FILE *hFile, void *lpBuffer, uint32_t nNumberOfBytesToRead, uint32_t *lpNumberOfBytesRead)
{
    uint32_t bcount;

    *lpNumberOfBytesRead = 0;

    while (nNumberOfBytesToRead) {
        bcount = (uint32_t) fread ((unsigned char *) lpBuffer + *lpNumberOfBytesRead, 1, nNumberOfBytesToRead, hFile);

        if (bcount) {
            *lpNumberOfBytesRead += bcount;
            nNumberOfBytesToRead -= bcount;
        }
        else
            break;
    }

    return !ferror (hFile);
}

int DoWriteFile (FILE *hFile, void *lpBuffer, uint32_t nNumberOfBytesToWrite, uint32_t *lpNumberOfBytesWritten)
{
    uint32_t bcount;

    *lpNumberOfBytesWritten = 0;

    while (nNumberOfBytesToWrite) {
        bcount = (uint32_t) fwrite ((unsigned char *) lpBuffer + *lpNumberOfBytesWritten, 1, nNumberOfBytesToWrite, hFile);

        if (bcount) {
            *lpNumberOfBytesWritten += bcount;
            nNumberOfBytesToWrite -= bcount;
        }
        else
            break;
    }

    return !ferror (hFile);
}

#ifdef _WIN32

int64_t DoGetFileSize (FILE *hFile)
{
    LARGE_INTEGER Size;
    HANDLE        fHandle;

    if (hFile == NULL)
        return 0;

    fHandle = (HANDLE)_get_osfhandle(_fileno(hFile));

    if (fHandle == INVALID_HANDLE_VALUE || GetFileType(fHandle) != FILE_TYPE_DISK)
        return 0;

    Size.u.LowPart = GetFileSize(fHandle, (DWORD *) &Size.u.HighPart);

    if (Size.u.LowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return 0;

    return (int64_t)Size.QuadPart;
}

int64_t DoGetFilePosition (FILE *hFile)
{
    return _ftelli64 (hFile);
}

int DoSetFilePositionAbsolute (FILE *hFile, int64_t pos)
{
    return _fseeki64 (hFile, pos, SEEK_SET);
}

int DoSetFilePositionRelative (FILE *hFile, int64_t pos, int mode)
{
    return _fseeki64 (hFile, pos, mode);
}

#else

int64_t DoGetFileSize (FILE *hFile)
{
    struct stat statbuf;

    if (!hFile || fstat (fileno (hFile), &statbuf) || !S_ISREG(statbuf.st_mode))
        return 0;

    return (int64_t) statbuf.st_size;
}

int64_t DoGetFilePosition (FILE *hFile)
{
    return ftell (hFile);
}

int DoSetFilePositionAbsolute (FILE *hFile, int64_t pos)
{
    return fseek (hFile, pos, SEEK_SET);
}

int DoSetFilePositionRelative (FILE *hFile, int64_t pos, int mode)
{
    return fseek (hFile, pos, mode);
}

#endif

// if ungetc() is not available, a seek of -1 is fine also because we do not
// change the byte read.

int DoUngetc (int c, FILE *hFile)
{
    return ungetc (c, hFile);
}

int DoCloseHandle (FILE *hFile)
{
    return hFile ? !fclose (hFile) : 0;
}

int DoTruncateFile (FILE *hFile)
{
    if (hFile) {
        fflush (hFile);
#if defined(_WIN32)
        return !_chsize (_fileno (hFile), 0);
#else
        return !ftruncate(fileno (hFile), 0);
#endif
    }

    return 0;
}

int DoDeleteFile (char *filename)
{
    return filename ? !remove (filename) : 0;
}

/////////////////////////////////////////////////////////////////////////////////
// Function to set the name of the console window. This is very handy for      //
// displaying progress of batch operations with the console window minimized.  //
/////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

void DoSetConsoleTitle (char *text)
{
    SetConsoleTitle (text);
}

#else

void DoSetConsoleTitle (char *text)
{
    fprintf (stderr, "\033]0;%s\007", text);
    fflush (stderr);
}

#endif

//////////////////////////////////////////////////////////////////////////////
// Apple 'chan' chunk (channel layout) support shared by the CAF and AIFF   //
// command-line modules. The parsing and generating of the big-endian Core  //
// Audio AudioChannelLayout lived in the CAF modules only; it is factored   //
// out here so that AIFF files can carry exactly the same layout            //
// information (Apple defines the identical 'chan' chunk for AIFF/AIFC).    //
//////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#define strdup(x) _strdup(x)
#endif

static const char TMH_full [] = { 1,2,3,13,9,10,5,6,12,14,15,16,17,9,4,18,7,8,19,20,21,0 };
static const char TMH_std [] = { 1,2,3,11,8,9,5,6,10,12,13,14,15,7,4,16,0 };

static struct {
    uint32_t mChannelLayoutTag;     // Core Audio layout, 100 - 146 in high word, num channels in low word
    uint32_t mChannelBitmap;        // Microsoft standard mask (for those channels that appear)
    const char *mChannelReorder;    // reorder string if layout is NOT in Microsoft standard order
    const char *mChannelIdentities; // identities of any channels NOT in Microsoft standard
} apple_layouts [] = {
    { (100<<16) | 1, 0x004, NULL,       NULL            },  // FC
    { (101<<16) | 2, 0x003, NULL,       NULL            },  // FL, FR
    { (102<<16) | 2, 0x003, NULL,       NULL            },  // FL, FR (headphones)
    { (103<<16) | 2, 0x000, NULL,       "\46\47"        },  // [Lt, Rt] (matrix encoded)
    { (104<<16) | 2, 0x000, NULL,       "\314\315"      },  // [Mid, Side]
    { (105<<16) | 2, 0x000, NULL,       "\316\317"      },  // [X, Y]
    { (106<<16) | 2, 0x003, NULL,       NULL            },  // FL, FR (binaural)
    { (107<<16) | 4, 0x000, NULL,       "\310\311\312\313"  },  // [W, X, Y, Z] (ambisonics)
    { (108<<16) | 4, 0x033, NULL,       NULL            },  // FL, FR, BL, BR (quad)
    { (109<<16) | 5, 0x037, "12453",    NULL            },  // FL, FR, BL, BR, FC (pentagonal)
    { (110<<16) | 6, 0x137, "124536",   NULL            },  // FL, FR, BL, BR, FC, BC (hexagonal)
    { (111<<16) | 8, 0x737, "12453678", NULL            },  // FL, FR, BL, BR, FC, BC, SL, SR (octagonal)
    { (112<<16) | 8, 0x2d033, NULL,     NULL            },  // FL, FR, BL, BR, TFL, TFR, TBL, TBR (cubic)
    { (113<<16) | 3, 0x007, NULL,       NULL            },  // FL, FR, FC
    { (114<<16) | 3, 0x007, "312",      NULL            },  // FC, FL, FR
    { (115<<16) | 4, 0x107, NULL,       NULL            },  // FL, FR, FC, BC
    { (116<<16) | 4, 0x107, "3124",     NULL            },  // FC, FL, FR, BC
    { (117<<16) | 5, 0x037, NULL,       NULL            },  // FL, FR, FC, BL, BR
    { (118<<16) | 5, 0x037, "12453",    NULL            },  // FL, FR, BL, BR, FC
    { (119<<16) | 5, 0x037, "13245",    NULL            },  // FL, FC, FR, BL, BR
    { (120<<16) | 5, 0x037, "31245",    NULL            },  // FC, FL, FR, BL, BR
    { (121<<16) | 6, 0x03f, NULL,       NULL            },  // FL, FR, FC, LFE, BL, BR
    { (122<<16) | 6, 0x03f, "125634",   NULL            },  // FL, FR, BL, BR, FC, LFE
    { (123<<16) | 6, 0x03f, "132564",   NULL            },  // FL, FC, FR, BL, BR, LFE
    { (124<<16) | 6, 0x03f, "312564",   NULL            },  // FC, FL, FR, BL, BR, LFE
    { (125<<16) | 7, 0x13f, NULL,       NULL            },  // FL, FR, FC, LFE, BL, BR, BC
    { (126<<16) | 8, 0x0ff, NULL,       NULL            },  // FL, FR, FC, LFE, BL, BR, FLC, FRC
    { (127<<16) | 8, 0x0ff, "37812564", NULL            },  // FC, FLC, FRC, FL, FR, BL, BR, LFE
    { (128<<16) | 8, 0x03f, NULL,       "\41\42"        },  // FL, FR, FC, LFE, BL, BR, [Rls, Rrs]
    { (129<<16) | 8, 0x0ff, "12563478", NULL            },  // FL, FR, BL, BR, FC, LFE, FLC, FRC
    { (130<<16) | 8, 0x03f, NULL,       "\46\47"        },  // FL, FR, FC, LFE, BL, BR, [Lt, Rt]
    { (131<<16) | 3, 0x103, NULL,       NULL            },  // FL, FR, BC
    { (132<<16) | 4, 0x033, NULL,       NULL            },  // FL, FR, BL, BR
    { (133<<16) | 3, 0x00B, NULL,       NULL            },  // FL, FR, LFE
    { (134<<16) | 4, 0x10B, NULL,       NULL            },  // FL, FR, LFE, BC
    { (135<<16) | 5, 0x03B, NULL,       NULL            },  // FL, FR, LFE, BL, BR
    { (136<<16) | 4, 0x00F, NULL,       NULL            },  // FL, FR, FC, LFE
    { (137<<16) | 5, 0x10f, NULL,       NULL            },  // FL, FR, FC, LFE, BC
    { (138<<16) | 5, 0x03b, "12453",    NULL            },  // FL, FR, BL, BR, LFE
    { (139<<16) | 6, 0x137, "124536",   NULL            },  // FL, FR, BL, BR, FC, BC
    { (140<<16) | 7, 0x037, "1245367",  "\41\42"        },  // FL, FR, BL, BR, FC, [Rls, Rrs]
    { (141<<16) | 6, 0x137, "312456",   NULL            },  // FC, FL, FR, BL, BR, BC
    { (142<<16) | 7, 0x13f, "3125674",  NULL            },  // FL, FR, BL, BR, BC, LFE
    { (143<<16) | 7, 0x037, "3124567",  "\41\42"        },  // FL, FR, BL, BR, [Rls, Rrs]
    { (144<<16) | 8, 0x137, "31245786", "\41\42"        },  // FL, FR, BL, BR, [Rls, Rrs], BC
    { (145<<16) | 16, 0x773f, TMH_std,  "\43\44\54\45"  },  // FL, FR, FC, TFC, SL, SR, BL, BR, TFL, TFR, [Lw, Rw, Csd], BC, LFE, [LFE2]
    { (146<<16) | 21, 0x77ff, TMH_full, "\43\44\54\45"  },  // FL, FR, FC, TFC, SL, SR, BL, BR, TFL, TFR, [Lw, Rw, Csd], BC, LFE, [LFE2],
                                                            //     FLC, FRC, [HI, VI, Haptic]
};

#define NUM_APPLE_LAYOUTS (sizeof (apple_layouts) / sizeof (apple_layouts [0]))

// Parse the body of an Apple 'chan' chunk (already read into memory) and apply it to the
// WavpackConfig in preparation for WavpackSetConfiguration64(). The channel mask and the
// QMODE_REORDERED_CHANS flag go directly into the config; the channel layout tag (or
// description count), the reorder array, and the non-Microsoft identity string are returned
// to the caller to be passed to WavpackSetChannelLayout() / WavpackSetConfiguration64()
// (the caller also frees the two allocated arrays). Returns WAVPACK_SOFT_ERROR on a
// malformed chunk, WAVPACK_NO_ERROR otherwise (including recognized-but-ignored chunks).

int ParseAppleChannelLayout (void *chunk_data, uint32_t chunk_size, WavpackConfig *config,
    uint32_t *channel_layout, unsigned char **channel_reorder, unsigned char **channel_identities)
{
    AppleChannelLayout *apple_channel_layout = chunk_data;
    int i;

    if (chunk_size < sizeof (AppleChannelLayout) || chunk_size > 1024) {
        error_line ("invalid 'chan' chunk size!");
        return WAVPACK_SOFT_ERROR;
    }

    if (config->channel_mask || (config->qmode & QMODE_CHANS_UNASSIGNED)) {
        error_line ("this file already has channel order information!");
        return WAVPACK_SOFT_ERROR;
    }

    WavpackBigEndianToNative (apple_channel_layout, AppleChannelLayoutFormat);

    switch (apple_channel_layout->mChannelLayoutTag) {
        case kAppleChannelLayoutTag_UseChannelDescriptions:
            {
                AppleChannelDescription *descriptions = (AppleChannelDescription *) (apple_channel_layout + 1);
                int num_descriptions = apple_channel_layout->mNumberChannelDescriptions;
                int label, cindex = 0, idents = 0;

                if (chunk_size != sizeof (AppleChannelLayout) + sizeof (AppleChannelDescription) * num_descriptions ||
                    num_descriptions != config->num_channels) {
                        error_line ("channel descriptions in 'chan' chunk are the wrong size!");
                        return WAVPACK_SOFT_ERROR;
                }

                if (num_descriptions >= 256) {
                    error_line ("%d channel descriptions is more than we can handle...ignoring!", num_descriptions);
                    break;
                }

                // we allocate (and initialize to invalid values) a channel reorder array
                // (even though we might not end up doing any reordering) and a string for
                // any non-Microsoft channels we encounter

                *channel_reorder = malloc (num_descriptions);
                memset (*channel_reorder, -1, num_descriptions);
                *channel_identities = malloc (num_descriptions+1);

                // convert the descriptions array to our native endian so it's easy to access

                for (i = 0; i < num_descriptions; ++i) {
                    WavpackBigEndianToNative (descriptions + i, AppleChannelDescriptionFormat);

                    if (debug_logging_mode)
                        error_line ("chan %d --> %d", i + 1, descriptions [i].mChannelLabel);
                }

                // first, we go though and find any MS channels present, and move those to the beginning

                for (label = 1; label <= 18; ++label)
                    for (i = 0; i < num_descriptions; ++i)
                        if (descriptions [i].mChannelLabel == label) {
                            config->channel_mask |= 1 << (label - 1);
                            (*channel_reorder) [i] = cindex++;
                            break;
                        }

                // next, we go though the channels again assigning any we haven't done

                for (i = 0; i < num_descriptions; ++i)
                    if ((*channel_reorder) [i] == (unsigned char) -1) {
                        uint32_t clabel = descriptions [i].mChannelLabel;

                        if (clabel == 0 || clabel == 0xffffffff || clabel == 100)
                            (*channel_identities) [idents++] = 0xff;
                        else if ((clabel >= 33 && clabel <= 44) || (clabel >= 200 && clabel <= 207) || (clabel >= 301 && clabel <= 305))
                            (*channel_identities) [idents++] = clabel >= 301 ? clabel - 80 : clabel;
                        else {
                            error_line ("warning: unknown channel descriptions label: %d", clabel);
                            (*channel_identities) [idents++] = 0xff;
                        }

                        (*channel_reorder) [i] = cindex++;
                    }

                // then, go through the reordering array and see if we really have to reorder

                for (i = 0; i < num_descriptions; ++i)
                    if ((*channel_reorder) [i] != i)
                        break;

                if (i == num_descriptions) {
                    free (*channel_reorder);                // no reordering required, so don't
                    *channel_reorder = NULL;
                }
                else {
                    config->qmode |= QMODE_REORDERED_CHANS; // reordering required, put channel count into layout
                    *channel_layout = num_descriptions;
                }

                if (!idents) {                              // if no non-MS channels, free the identities string
                    free (*channel_identities);
                    *channel_identities = NULL;
                }
                else
                    (*channel_identities) [idents] = 0;     // otherwise NULL terminate it

                if (debug_logging_mode) {
                    error_line ("layout_tag = 0x%08x, so generated bitmap of 0x%08x from %d descriptions, %d non-MS",
                        apple_channel_layout->mChannelLayoutTag, config->channel_mask,
                        apple_channel_layout->mNumberChannelDescriptions, idents);

                    // if debugging, display the reordering as a string (but only little ones)

                    if (*channel_reorder && num_descriptions <= 8) {
                        char reorder_string [] = "12345678";

                        for (i = 0; i < num_descriptions; ++i)
                            reorder_string [i] = (*channel_reorder) [i] + '1';

                        reorder_string [i] = 0;
                        error_line ("reordering string = \"%s\"\n", reorder_string);
                    }
                }
            }

            break;

        case kAppleChannelLayoutTag_UseChannelBitmap:
            config->channel_mask = apple_channel_layout->mChannelBitmap;

            if (debug_logging_mode)
                error_line ("layout_tag = 0x%08x, so using supplied bitmap of 0x%08x",
                    apple_channel_layout->mChannelLayoutTag, apple_channel_layout->mChannelBitmap);

            break;

        default:
            for (i = 0; i < NUM_APPLE_LAYOUTS; ++i)
                if (apple_channel_layout->mChannelLayoutTag == apple_layouts [i].mChannelLayoutTag) {
                    config->channel_mask = apple_layouts [i].mChannelBitmap;
                    *channel_layout = apple_layouts [i].mChannelLayoutTag;

                    if (apple_layouts [i].mChannelReorder) {
                        *channel_reorder = (unsigned char *) strdup (apple_layouts [i].mChannelReorder);
                        config->qmode |= QMODE_REORDERED_CHANS;
                    }

                    if (apple_layouts [i].mChannelIdentities)
                        *channel_identities = (unsigned char *) strdup (apple_layouts [i].mChannelIdentities);

                    if (debug_logging_mode)
                        error_line ("layout_tag 0x%08x found in table, bitmap = 0x%08x, reorder = %s, identities = %s",
                            *channel_layout, config->channel_mask, *channel_reorder ? "yes" : "no", *channel_identities ? "yes" : "no");

                    break;
                }

            if (i == NUM_APPLE_LAYOUTS && debug_logging_mode)
                error_line ("layout_tag 0x%08x not found in table...all channels unassigned",
                    apple_channel_layout->mChannelLayoutTag);

            break;
    }

    return WAVPACK_NO_ERROR;
}

// Determine whether a 'chan' chunk should be written for the specified WavPack file
// and, if so, how big its body will be (zero means no chunk should be written). This
// decision and the body generation in WriteAppleChannelLayoutBody() must match exactly.

uint32_t AppleChannelLayoutBodySize (WavpackContext *wpc, int qmode)
{
    uint32_t channel_layout_tag = WavpackGetChannelLayout (wpc, NULL);
    int32_t channel_mask = WavpackGetChannelMask (wpc);
    int num_channels = WavpackGetNumChannels (wpc);
    int num_identified_chans = 0, i;
    unsigned char *channel_identities;
    unsigned int bits = 0, bmask;

    // we write the Channel Layout Chunk if any of these are true:
    // 1. a specific Apple layout was specified (100 - 146)
    // 2. there are more than 2 channels and ANY are defined
    // 3. there are 1 or 2 channels and NOT regular mono/stereo

    channel_identities = malloc (num_channels + 1);
    WavpackGetChannelIdentities (wpc, channel_identities);

    for (i = 0; i < num_channels; ++i)
        if (channel_identities [i] != 0xff)
            num_identified_chans++;

    free (channel_identities);

    if (!(channel_layout_tag || (num_channels > 2 ? num_identified_chans : channel_mask != 5 - num_channels)))
        return 0;

    for (bmask = 1; bmask; bmask <<= 1)     // count the set bits in the channel mask
        if (bmask & channel_mask)
            ++bits;

    // we use a layout tag if there is a specific Apple layout (100 - 146) or all the
    // channels are MS defined and in MS order...otherwise we have to write a full
    // channel description array

    if ((channel_layout_tag & 0xff0000) || (bits == num_channels && !(qmode & QMODE_REORDERED_CHANS)))
        return sizeof (AppleChannelLayout);
    else
        return sizeof (AppleChannelLayout) + sizeof (AppleChannelDescription) * num_channels;
}

// Write the body of a 'chan' chunk for the specified WavPack file (the caller writes the
// container-specific chunk header). Returns TRUE on success, FALSE on a write error.

int WriteAppleChannelLayoutBody (FILE *outfile, WavpackContext *wpc, int qmode)
{
    uint32_t channel_layout_tag = WavpackGetChannelLayout (wpc, NULL);
    int32_t channel_mask = WavpackGetChannelMask (wpc);
    int num_channels = WavpackGetNumChannels (wpc);
    unsigned char *channel_identities = malloc (num_channels + 1);
    unsigned char *new_channel_order = NULL;
    AppleChannelLayout apple_channel_layout;
    unsigned int bits = 0, bmask;
    uint32_t bcount;
    int i;

    for (bmask = 1; bmask; bmask <<= 1)     // count the set bits in the channel mask
        if (bmask & channel_mask)
            ++bits;

    WavpackGetChannelIdentities (wpc, channel_identities);

    // we use a layout tag if there is a specific Apple layout (100 - 146) or all the
    // channels are MS defined and in MS order...otherwise we have to write a full
    // channel description array

    if ((channel_layout_tag & 0xff0000) || (bits == num_channels && !(qmode & QMODE_REORDERED_CHANS))) {
        if (channel_layout_tag) {
            if (debug_logging_mode)
                error_line ("writing \"chan\" chunk with layout tag 0x%08x", channel_layout_tag);

            apple_channel_layout.mChannelLayoutTag = channel_layout_tag;
            apple_channel_layout.mChannelBitmap = 0;
        }
        else {
            if (debug_logging_mode)
                error_line ("writing \"chan\" chunk with UseChannelBitmap tag, bitmap = 0x%08x", channel_mask);

            apple_channel_layout.mChannelLayoutTag = kAppleChannelLayoutTag_UseChannelBitmap;
            apple_channel_layout.mChannelBitmap = channel_mask;
        }

        apple_channel_layout.mNumberChannelDescriptions = 0;
        WavpackNativeToBigEndian (&apple_channel_layout, AppleChannelLayoutFormat);

        if (!DoWriteFile (outfile, &apple_channel_layout, sizeof (apple_channel_layout), &bcount) ||
            bcount != sizeof (apple_channel_layout)) {
                free (channel_identities);
                return FALSE;
        }
    }
    else {  // write a channel description array because a single layout or bitmap won't do it...
        AppleChannelDescription apple_channel_description;

        if (debug_logging_mode)
            error_line ("writing \"chan\" chunk with UseChannelDescriptions tag, bitmap = 0x%08x, reordered = %s",
                channel_mask, (qmode & QMODE_REORDERED_CHANS) ? "yes" : "no");

        if (qmode & QMODE_REORDERED_CHANS) {
            if ((int)(channel_layout_tag & 0xff) <= num_channels) {
                new_channel_order = malloc (num_channels);

                for (i = 0; i < num_channels; ++i)
                    new_channel_order [i] = i;

                WavpackGetChannelLayout (wpc, new_channel_order);
            }
        }

        apple_channel_layout.mChannelLayoutTag = kAppleChannelLayoutTag_UseChannelDescriptions;
        apple_channel_layout.mChannelBitmap = 0;
        apple_channel_layout.mNumberChannelDescriptions = num_channels;
        WavpackNativeToBigEndian (&apple_channel_layout, AppleChannelLayoutFormat);

        if (!DoWriteFile (outfile, &apple_channel_layout, sizeof (apple_channel_layout), &bcount) ||
            bcount != sizeof (apple_channel_layout)) {
                free (new_channel_order);
                free (channel_identities);
                return FALSE;
        }

        for (i = 0; i < num_channels; ++i) {
            unsigned char chan_id = new_channel_order ? channel_identities [new_channel_order [i]] : channel_identities [i];
            CLEAR (apple_channel_description);

            if ((chan_id >= 1 && chan_id <= 18) || (chan_id >= 33 && chan_id <= 44) || (chan_id >= 200 && chan_id <= 207))
                apple_channel_description.mChannelLabel = chan_id;
            else if (chan_id >= 221 && chan_id <= 225)
                apple_channel_description.mChannelLabel = chan_id + 80;

            if (debug_logging_mode)
                error_line ("chan %d --> %d", i + 1, apple_channel_description.mChannelLabel);

            WavpackNativeToBigEndian (&apple_channel_description, AppleChannelDescriptionFormat);

            if (!DoWriteFile (outfile, &apple_channel_description, sizeof (apple_channel_description), &bcount) ||
                bcount != sizeof (apple_channel_description)) {
                    free (new_channel_order);
                    free (channel_identities);
                    return FALSE;
            }
        }
    }

    free (new_channel_order);
    free (channel_identities);
    return TRUE;
}
