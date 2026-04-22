#include <stdio.h>
#include <stdlib.h>
#include "../../include/interface.h"

/*
 * input_read_file
 * Read a whole file and return null-terminated SQL text.
 * Caller must free() the returned string.
 */
char *input_read_file(const char *path)
{
    if (!path)
        return NULL;

    // 파일을 오픈 read byte
    FILE *fp = fopen(path, "rb");
    // 파일이 존재하지 않으면 에러
    if (!fp)
    {
        fprintf(stderr, "input: cannot open file '%s'\n", path);
        return NULL;
    }

    // 파일을 돌아서 마지막 위치인 경우까지 체크
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return NULL;
    }

    // fopen 함수를 통해 열린 file stream size를 확인
    long size = ftell(fp);
    if (size < 0)
    {
        fclose(fp);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    if (ferror(fp) || read != (size_t)size)
    {
        free(buf);
        fclose(fp);
        return NULL;
    }

    buf[read] = '\0';
    fclose(fp);
    return buf;
}
