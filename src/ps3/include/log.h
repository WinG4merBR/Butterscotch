#include <stdio.h>

void logger(const char* gameName, const char* message) {
    FILE* f = fopen("/dev_hdd0/tmp/butterscotch.log", "a");
    
    if (f) {
        fprintf(f, "[%s] %s\n", gameName, message);
        fclose(f);
    }
}