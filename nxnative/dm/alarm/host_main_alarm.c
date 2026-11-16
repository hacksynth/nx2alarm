// main() of nx2alarm-host.
// The upstream host core's entry point is dm/host/main.c, which main.c keeps unchanged: build_host.py compiles that file with
// -Dmain=dm_host_main, and this wrapper gives the process its real entry point. Keeping it here means the Linux port needs no
// edit of the shared host code (the iOS front end calls dm_host_preload_start from its own main and keeps main.c as it is).
int dm_host_main(int argc, char **argv);

int main(int argc, char **argv) {
    return dm_host_main(argc, argv);
}
