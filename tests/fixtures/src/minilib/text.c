unsigned lib_strlen(const char* s) {
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

int lib_count_char(const char* s, char c) {
    int n = 0;
    for (; *s; ++s) n += *s == c;
    return n;
}
