// The exception-handling fixture's entry point; its functions are the corpus's (tests/corpus/eh.cpp,
// seh.c and eh_rt.c). Only linked and analyzed, never run.
int eh_corpus(int code);
int seh_corpus(const int* p, const int* q);

int entry(void) {
    int v = 3;
    return eh_corpus(v) + seh_corpus(&v, &v);
}
