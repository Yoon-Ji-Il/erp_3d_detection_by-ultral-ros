from setuptools import find_packages
from setuptools import setup

setup(
    name='sensor_msgs',
    version='4.9.0',
    packages=find_packages(
        include=('sensor_msgs', 'sensor_msgs.*')),
)
