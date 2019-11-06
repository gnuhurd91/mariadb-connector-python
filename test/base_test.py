#!/usr/bin/env python -O
# -*- coding: utf-8 -*-

from .conf_test import conf
import mariadb

def create_connection(additional_conf = None):
    default_conf = conf()
    if additional_conf is None:
        c = {key: value for (key, value) in (default_conf.items())}
    else:
        c = {key: value for (key, value) in (default_conf.items() + additional_conf.items())}
    return mariadb.connect(**c)
