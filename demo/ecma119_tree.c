/*
 * Little program that imports a directory to iso image, generates the
 * ecma119 low level tree and prints it.
 * Note that this is not an API example, but a little program for test
 * purposes.
 */

#include "libisofs.h"
#include "ecma119.h"
#include "ecma119_tree.h"
#include "util.h"
#include "filesrc.h"
#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static void
print_permissions(mode_t mode)
{
    char perm[10];

    //TODO suid, sticky...

    perm[9] = '\0';
    perm[8] = mode & S_IXOTH ? 'x' : '-';
    perm[7] = mode & S_IWOTH ? 'w' : '-';
    perm[6] = mode & S_IROTH ? 'r' : '-';
    perm[5] = mode & S_IXGRP ? 'x' : '-';
    perm[4] = mode & S_IWGRP ? 'w' : '-';
    perm[3] = mode & S_IRGRP ? 'r' : '-';
    perm[2] = mode & S_IXUSR ? 'x' : '-';
    perm[1] = mode & S_IWUSR ? 'w' : '-';
    perm[0] = mode & S_IRUSR ? 'r' : '-';
    printf("[%s]",perm);
}

static void
print_dir(Ecma119Node *dir, int level)
{
    int i;
    char *sp = alloca(level * 2 + 1);

    for (i = 0; i < level * 2; i += 2) {
        sp[i] = '|';
        sp[i+1] = ' ';
    }

    sp[level * 2-1] = '-';
    sp[level * 2] = '\0';

    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Ecma119Node *child = dir->info.dir->children[i];

        if (child->type == ECMA119_DIR) {
            printf("%s+[D] ", sp);
            print_permissions(iso_node_get_permissions(child->node));
            printf(" %s\n", child->iso_name);
            print_dir(child, level+1);
        } else if (child->type == ECMA119_FILE) {
            printf("%s-[F] ", sp);
            print_permissions(iso_node_get_permissions(child->node));
            printf(" %s {%p}\n", child->iso_name, (void*)child->info.file);
        } else if (child->type == ECMA119_SYMLINK) {
            printf("%s-[L] ", sp);
            print_permissions(iso_node_get_permissions(child->node));
            printf(" %s -> %s\n", child->iso_name,
                   ((IsoSymlink*)child->node)->dest);
        } else if (child->type == ECMA119_SPECIAL) {
            printf("%s-[S] ", sp);
            print_permissions(iso_node_get_permissions(child->node));
            printf(" %s\n", child->iso_name);
        } else if (child->type == ECMA119_PLACEHOLDER) {
            printf("%s-[RD] ", sp);
            print_permissions(iso_node_get_permissions(child->node));
            printf(" %s\n", child->iso_name);
        } else {
            printf("%s-[????] ", sp);
        }
    }
}

int main(int argc, char **argv)
{
    int result;
    IsoImage *image;
    Ecma119Image *ecma119;

    if (argc != 2) {
        printf ("You need to specify a valid path\n");
        return 1;
    }

    iso_init();
    iso_set_msgs_severities("NEVER", "ALL", "");
    result = iso_image_new("volume_id", &image);
    if (result < 0) {
        printf ("Error creating image\n");
        return 1;
    }

    result = iso_tree_add_dir_rec(image, iso_image_get_root(image), argv[1]);
    if (result < 0) {
        printf ("Error adding directory %d\n", result);
        return 1;
    }

    ecma119 = calloc(1, sizeof(Ecma119Image));
    iso_rbtree_new(iso_file_src_cmp, &(ecma119->files));
    ecma119->iso_level = 1;
    ecma119->rockridge = 1;
    ecma119->image = image;
    ecma119->input_charset = strdup("UTF-8");

    /* create low level tree */
    result = ecma119_tree_create(ecma119);
    if (result < 0) {
        printf ("Error creating ecma-119 tree: %d\n", result);
        return 1;
    }

    printf("================= ECMA-119 TREE =================\n");
    print_dir(ecma119->root, 0);
    printf("\n\n");

    ecma119_node_free(ecma119->root);
    iso_rbtree_destroy(ecma119->files, iso_file_src_free);
    free(ecma119->input_charset);
    free(ecma119);
    iso_image_unref(image);
    iso_finish();
    return 0;
}
