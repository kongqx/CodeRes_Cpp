#include <unistd.h>
#include <stdio.h>

int main()
{
    unsigned int i = 0;

    while( 1 ) {
        ++i;
        write( STDOUT_FILENO, &i, sizeof(i) );
        fprintf(stderr, "write %d\n", i);
        /* sleep(1); */
    } // while

    return 0;
}
