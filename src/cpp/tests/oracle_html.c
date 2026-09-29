/* Host test driver: renders stdin with the vendored md4c and md4c-html using
 * the fixture oracle options (md2html --gfm). */

#include <stdio.h>
#include <stdlib.h>

#include "md4c-html.h"

static void write_output(const MD_CHAR *text, MD_SIZE size, void *userdata) {
  (void)userdata;
  fwrite(text, 1, size, stdout);
}

int main(void) {
  size_t capacity = 1 << 16;
  size_t size = 0;
  char *buffer = malloc(capacity);
  size_t read;
  while ((read = fread(buffer + size, 1, capacity - size, stdin)) > 0) {
    size += read;
    if (size == capacity) {
      capacity *= 2;
      buffer = realloc(buffer, capacity);
    }
  }
  int status = md_html(buffer, (MD_SIZE)size, write_output, NULL, MD_DIALECT_GITHUB, 0);
  free(buffer);
  return status == 0 ? 0 : 1;
}
