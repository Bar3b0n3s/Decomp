// Nothing in the program refers to this member, so the linker leaves it out.
int lib_unused(int a) {
    int r = 1;
    while (a-- > 0) r = r * 3 + 1;
    return r;
}
