#if defined(__LD_SCRIPT__) || defined(__ASSEMBLER__)
#define ULL_NUM(x) x
#else
#define ULL_NUM(x) (x##ULL)
#endif

#define LD_START_ADDRESS ULL_NUM(0)
#define LD_STACK_SIZE ULL_NUM(0x40000)
