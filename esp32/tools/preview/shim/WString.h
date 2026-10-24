#pragma once
#include <string.h>

/// Just enough of Arduino's String for Adafruit_GFX's String overloads.
class String {
public:
    String(const char* text = "") : text_(text) {}
    unsigned length() const { return static_cast<unsigned>(strlen(text_)); }
    char charAt(unsigned index) const { return text_[index]; }
    const char* c_str() const { return text_; }

private:
    const char* text_;
};
