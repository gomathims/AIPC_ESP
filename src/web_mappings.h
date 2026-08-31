#ifndef WEB_MAPPINGS_H
#define WEB_MAPPINGS_H

#include <map>
#include <Arduino.h>
#include "web_content.h"

std::map<String, const char*> stringMap = {
    {"css/style.css", css_style_css},
    {"css/style_additional.css", css_style_additional_css},
    {"images/alert.svg", images_alert_svg},
    {"images/wifi_e.svg", images_wifi_e_svg},
    {"images/wifi_g.svg", images_wifi_g_svg},
    {"images/wifi_w.svg", images_wifi_w_svg},
    {"index.html", index_html},
    {"log.html", log_html},
    {"login.html", login_html},
    {"scripts/app.js", scripts_app_js},
};

struct BinaryFile {
    const unsigned char* data;
    unsigned int length;
};

std::map<String, BinaryFile> binaryMap = {
    {"images/ap.png", {images_ap_png, images_ap_png_len}},
    {"images/favicon.ico", {images_favicon_ico, images_favicon_ico_len}},
    {"images/home.png", {images_home_png, images_home_png_len}},
    {"images/logo.png", {images_logo_png, images_logo_png_len}},
    {"images/power.png", {images_power_png, images_power_png_len}},
    {"images/settings.png", {images_settings_png, images_settings_png_len}},
    {"favicon.ico", {images_favicon_ico, images_favicon_ico_len}},
};

#endif // WEB_MAPPINGS_H
