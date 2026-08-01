#include "../../src/http_home_page.h"

#include <stdio.h>

int main()
{
	char body[16384];
	const size_t length = aius_http_home_page(body, sizeof(body), "test-revision", "2026-07-31T00:00:00Z");
	if (length == 0u)
		return 1;
	fwrite(body, 1, length, stdout);
	return 0;
}
