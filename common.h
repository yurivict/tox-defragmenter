
// symbol visibility macros
#define FUNC_LOCAL __attribute__ ((visibility ("hidden")))

// messages
#define WARNING(fmt...) {fprintf(stderr, "WARNING: Defragmenter: " fmt);}
