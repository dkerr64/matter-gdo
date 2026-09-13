/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// System build includes
#include <sdkconfig.h>

#if CONFIG_ENABLE_CHIP_SHELL
extern int gdo_command_handler(int argc, char **argv);
#endif // CONFIG_ENABLE_CHIP_SHELL
