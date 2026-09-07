# SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
#
# ESP32-S3 + W5500 Ethernet Test boot verification.
# Requires the W5500 hardware attached; otherwise the app stops after the
# SPI test failure with a clear error message.
import pytest
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize('target', ['esp32s3'], indirect=['target'])
def test_boot_banner(dut: IdfDut) -> None:
    dut.expect('ESP32-S3 + W5500 Ethernet Test')
