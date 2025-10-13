from setuptools import find_packages
from setuptools import setup

setup(
    name='nav_msgs',
    version='4.9.0',
    packages=find_packages(
        include=('nav_msgs', 'nav_msgs.*')),
)
