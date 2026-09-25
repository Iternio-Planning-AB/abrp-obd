#ifndef DUMMY_TEST_H
#define DUMMY_TEST_H

// A random string that will force the compiled binary to change
const char* DUMMY_OTA_TEST_STRING = "OTA_UPDATE_v7_ENCRYPT_TEST_AUGUST_21_2026";

// A nonsense math function
inline int calculate_nonsense_value() {
    int a = 42;
    int b = 100;
    return (a * b) + 7; // Returns 4207
}

#endif // DUMMY_TEST_H