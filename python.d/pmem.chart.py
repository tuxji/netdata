# -*- coding: utf-8 -*-
# Description: pmem netdata python.d module

from base import ExecutableService

# default module values (can be overridden per job in `config`)
# update_every = 2
priority = 60000
retries = 60

# charts order (can be overridden if you want less charts, or different order)
ORDER = ['pmem']

CHARTS = {
    'pmem': {
        'options': ['used', "Largest Process In Physical Memory", "% MEM", 'pmem', 'pmem.used', 'line'],
        'lines': [
            ['largest', None, 'absolute']
        ]}
}


class Service(ExecutableService):
    def __init__(self, configuration=None, name=None):
        ExecutableService.__init__(self, configuration=configuration, name=name)
        self.command = "ps -eo %mem= --sort=-%mem"
        self.order = ORDER
        self.definitions = CHARTS

    def check(self):
        """
        Parse basic configuration, check if command is returning values
        :return: boolean
        """
        if self.name is not None or self.name != str(None):
            self.name = ""
        else:
            self.name = str(self.name)
        try:
            self.command = str(self.configuration['command'])
        except (KeyError, TypeError):
            self.info("No command specified. Using: '" + self.command + "'")
        # Splitting self.command on every space so subprocess.Popen reads it properly
        self.command = self.command.split(' ')

        data = self._get_data()
        if data is None or len(data) == 0:
            self.error("Command returned no data")
            return False

        return True

    def _get_data(self):
        """
        Format data received from shell command
        :return: dict
        """
        try:
            return {'largest': float(self._get_raw_data()[0])}
        except (TypeError, ValueError, AttributeError):
            return None
