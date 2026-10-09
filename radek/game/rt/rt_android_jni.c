#include <jni.h>

#include "rt_entry.h"

JNIEXPORT jstring JNICALL
Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame(
    JNIEnv *environment, jclass activity_class, jstring memory_image_path,
    jstring guest_data_root, jstring guest_bundle_root, jstring guest_obb_root) {
    const char *memory_path;
    const char *data_root;
    const char *bundle_root = NULL;
    const char *obb_root = NULL;
    char report[16384];
    jstring result;
    (void)activity_class;

    if (memory_image_path == NULL || guest_data_root == NULL)
        return (*environment)->NewStringUTF(environment,
            "{\"execution\":{\"status\":\"SETUP_FAILED\"},"
            "\"reason\":\"translated memory or app-private data path is null\"}");
    memory_path = (*environment)->GetStringUTFChars(environment, memory_image_path, NULL);
    if (memory_path == NULL)
        return NULL;
    data_root = (*environment)->GetStringUTFChars(environment, guest_data_root, NULL);
    if (data_root == NULL) {
        (*environment)->ReleaseStringUTFChars(environment, memory_image_path, memory_path);
        return NULL;
    }
    if (guest_bundle_root != NULL) {
        bundle_root = (*environment)->GetStringUTFChars(environment, guest_bundle_root, NULL);
        if (bundle_root == NULL)
            goto release_required_paths;
    }
    if (guest_obb_root != NULL) {
        obb_root = (*environment)->GetStringUTFChars(environment, guest_obb_root, NULL);
        if (obb_root == NULL)
            goto release_required_paths;
    }

    (void)radek_translated_game_run(memory_path, data_root, bundle_root, obb_root,
                                    report, sizeof report);
    result = (*environment)->NewStringUTF(environment, report);

    if (obb_root != NULL)
        (*environment)->ReleaseStringUTFChars(environment, guest_obb_root, obb_root);
    if (bundle_root != NULL)
        (*environment)->ReleaseStringUTFChars(environment, guest_bundle_root, bundle_root);
    (*environment)->ReleaseStringUTFChars(environment, guest_data_root, data_root);
    (*environment)->ReleaseStringUTFChars(environment, memory_image_path, memory_path);
    return result;

release_required_paths:
    if (bundle_root != NULL)
        (*environment)->ReleaseStringUTFChars(environment, guest_bundle_root, bundle_root);
    (*environment)->ReleaseStringUTFChars(environment, guest_data_root, data_root);
    (*environment)->ReleaseStringUTFChars(environment, memory_image_path, memory_path);
    return NULL;
}
