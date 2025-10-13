from setuptools import find_packages
from setuptools import setup

setup(
    name='trajectory_msgs',
    version='4.9.0',
    packages=find_packages(
        include=('trajectory_msgs', 'trajectory_msgs.*')),
)
