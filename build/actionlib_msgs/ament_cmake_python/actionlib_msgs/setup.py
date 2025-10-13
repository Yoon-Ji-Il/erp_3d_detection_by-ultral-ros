from setuptools import find_packages
from setuptools import setup

setup(
    name='actionlib_msgs',
    version='4.9.0',
    packages=find_packages(
        include=('actionlib_msgs', 'actionlib_msgs.*')),
)
