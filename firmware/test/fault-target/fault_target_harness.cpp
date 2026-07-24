#include "aius_fault_teensy.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

bool parse_checkpoint(const char *text, std::uint16_t *checkpoint)
{
	char *end = nullptr;
	unsigned long value;

	if (text == nullptr || checkpoint == nullptr || *text == '\0') {
		return false;
	}
	errno = 0;
	value = std::strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    value > std::numeric_limits<std::uint16_t>::max()) {
		return false;
	}
	*checkpoint = static_cast<std::uint16_t>(value);
	return true;
}

void call_every_checkpoint()
{
	for (std::uint32_t checkpoint = 0;
	     checkpoint <= std::numeric_limits<std::uint16_t>::max();
	     ++checkpoint) {
		aius_fault_checkpoint(static_cast<std::uint16_t>(checkpoint));
	}
}

void call_every_checkpoint_except(std::uint16_t excluded)
{
	for (std::uint32_t checkpoint = 0;
	     checkpoint <= std::numeric_limits<std::uint16_t>::max();
	     ++checkpoint) {
		if (checkpoint != excluded) {
			aius_fault_checkpoint(static_cast<std::uint16_t>(checkpoint));
		}
	}
}

} // namespace

int main(int argc, char **argv)
{
	std::uint16_t checkpoint;

	if (argc == 2 && std::strcmp(argv[1], "--all") == 0) {
		std::puts("entered");
		std::fflush(stdout);
		call_every_checkpoint();
		std::puts("returned-all");
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "--except") == 0 &&
	    parse_checkpoint(argv[2], &checkpoint)) {
		std::puts("entered");
		std::fflush(stdout);
		call_every_checkpoint_except(checkpoint);
		std::puts("returned-except");
		return 0;
	}
	if (argc == 2 && parse_checkpoint(argv[1], &checkpoint)) {
		std::puts("entered");
		std::fflush(stdout);
		aius_fault_checkpoint(checkpoint);
		std::puts("returned");
		return 0;
	}
	return 2;
}
