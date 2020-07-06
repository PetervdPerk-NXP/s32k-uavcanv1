#!/bin/bash

nnvg --outdir include --templates templates -e .h public_regulated_data_types/uavcan/
nnvg --outdir include --templates templates -e .h public_regulated_data_types/regulated/ -I public_regulated_data_types/uavcan/
