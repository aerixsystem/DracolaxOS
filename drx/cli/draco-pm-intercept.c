/* draco-pm-intercept.c — Host-side package manager intercept wrapper
 * Compiled separately (host gcc, not the freestanding kernel build).
 * Install symlinks: apt -> draco-pm-intercept, pacman -> draco-pm-intercept, etc.
 * Build: gcc -O2 -o draco-pm-intercept draco-pm-intercept.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const struct { const char *alien; const char *draco; } pkg_map[] = {
    {"vim","draco-vim"},{"nano","draco-nano"},{"curl","draco-curl"},
    {"wget","draco-wget"},{"git","draco-git"},{"python3","draco-python3"},
    {"python","draco-python3"},{"gcc","draco-gcc"},{"make","draco-make"},
    {"htop","draco-htop"},{"firefox","draco-browser"},{"chromium","draco-browser"},
    {"lxqt","draco-lxqt"},{"lxqt-core","draco-lxqt"},{"openssl","draco-openssl"},
    {"openssh","draco-openssh"},{"alsa-utils","draco-alsa"},
    {"pulseaudio","draco-pulseaudio"},{"pipewire","draco-pipewire"},{NULL,NULL}
};

static const char *map_pkg(const char *n){
    for(int i=0;pkg_map[i].alien;i++) if(strcmp(pkg_map[i].alien,n)==0) return pkg_map[i].draco;
    return n;
}

int main(int argc, char **argv){
    const char *self=strrchr(argv[0],'/'); self=self?self+1:argv[0];
    int intercept=(strcmp(self,"apt")==0||strcmp(self,"apt-get")==0||
                   strcmp(self,"pacman")==0||strcmp(self,"pkg")==0||
                   strcmp(self,"yum")==0||strcmp(self,"dnf")==0||
                   strcmp(self,"brew")==0||strcmp(self,"snap")==0);
    if(intercept)
        printf("\033[1;33m[draco]\033[0m Intercepted '%s' — redirecting to draco install.\n",self);
    if(argc<2){printf("Usage: draco <install|remove|list|search|update> [pkg]\n");return 1;}
    const char *cmd=argv[1];
    if(strcmp(cmd,"install")==0||strcmp(cmd,"-S")==0||strcmp(cmd,"add")==0){
        for(int i=2;i<argc;i++){
            const char *d=map_pkg(argv[i]);
            printf("[draco install] Installing %s%s\n",d,strcmp(d,argv[i])?"  (mapped)":"");
        }
    } else if(strcmp(cmd,"remove")==0||strcmp(cmd,"-R")==0){
        for(int i=2;i<argc;i++) printf("[draco remove] Removing %s\n",map_pkg(argv[i]));
    } else if(strcmp(cmd,"update")==0||strcmp(cmd,"-Syu")==0||strcmp(cmd,"upgrade")==0){
        printf("[draco update] Package list refreshed.\n");
    } else if(strcmp(cmd,"list")==0){
        printf("[draco list] No packages installed yet.\n");
    } else if(strcmp(cmd,"search")==0&&argc>=3){
        for(int i=0;pkg_map[i].alien;i++)
            if(strstr(pkg_map[i].alien,argv[2])||strstr(pkg_map[i].draco,argv[2]))
                printf("  %s  ->  %s\n",pkg_map[i].alien,pkg_map[i].draco);
    } else { printf("Unknown command '%s'\n",cmd); return 1; }
    return 0;
}
