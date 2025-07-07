# SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

'''
Steps to run these cases:
- Build
  - . ${IDF_PATH}/export.sh
  - pip install idf_build_apps
  - python tools/build_apps.py components/sensor/radar/at581x/test_apps -t esp32s3
- Test
  - pip install -r tools/requirements/requirement.pytest.txt
  - pytest components/sensor/radar/at581x/test_apps --target esp32s3
'''

import pytest
from pytest_embedded import Dut

@pytest.mark.supported_targets
@pytest.mark.generic
def test_buzzer(dut: Dut)-> None:
    dut.run_all_single_board_cases()
