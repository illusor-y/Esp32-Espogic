// 网页静态资源总入口：CSS/HTML/JS 已拆分到同名子文件。
// 拼接顺序见 web_server.ino 的 getHTML()：
//   PAGE_HTML_HEAD_A -> PAGE_CSS -> PAGE_HTML_HEAD_B -> [动态时间]
//   -> PAGE_HTML_FOOT_A -> PAGE_JS_CORE -> PAGE_JS_IO -> PAGE_JS_COMPARATOR -> PAGE_JS_COUNTER -> PAGE_HTML_FOOT_B
#ifndef SMARTGLASS_WEBPAGE_H
#define SMARTGLASS_WEBPAGE_H

#include "web/webpage_css.h"
#include "web/webpage_html.h"
#include "web/webpage_js_core.h"
#include "web/webpage_js_io.h"
#include "web/webpage_js_comparator.h"
#include "web/webpage_js_counter.h"
#include "web/webpage_js_central.h"
#include "web/webpage_js_weather.h"

#endif
