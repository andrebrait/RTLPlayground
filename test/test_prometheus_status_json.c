/* /information.json reports the exporter setting in PROMETHEUS builds. */
#define main existing_page_json_main
#include "test_page_json.c"
#undef main
#include "metrics.h"

uint8_t prometheus_enabled;

int main(void)
{
	prometheus_enabled = 1;
	send_basic_info();
	CHECK(has("\"prometheus\":true") && balanced(), "enabled exporter is reported as true");
	prometheus_enabled = 0;
	send_basic_info();
	CHECK(has("\"prometheus\":false") && balanced(), "disabled exporter is reported as false");
	return tests_failed ? 1 : 0;
}
