"""
 Copyright (c) 2018 Intel Corporation

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
"""

import numpy as np

from mo.front.common.partial_infer.elemental import single_output_infer
from mo.front.common.partial_infer.reshape import tf_reshape_shape_infer


def tf_reshape_ext(pb):
    return {
        'type': 'Reshape',
        'infer': lambda node: single_output_infer(node, tf_reshape_shape_infer,
                                                  lambda node: np.reshape(node.in_node().value,
                                                                          node.out_node().shape) if node.in_node().value is not None else None)
    }
