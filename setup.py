try:
    from setuptools import setup, Extension
except ImportError:
    from distutils.core import setup, Extension

setup(name='peloton_bloomfilters_py3',
      author = 'Adam DePrince',
      author_email = 'adam@pelotoncycle.com',
      url = 'https://github.com/k4nar/peloton_bloomfilters',
      version='0.0.2',
      description="Peloton Cycle's Bloomin fast Bloomfilters - Python 2 & 3 compatibility",
      ext_modules=(
          [
              Extension(
                  name='peloton_bloomfilters',
                  sources=['peloton_bloomfiltersmodule.c']),
          ]
      ),
      classifiers=[
          "License :: OSI Approved :: Apache Software License",
          "Programming Language :: Python :: 2",
          "Programming Language :: Python :: 2.7",
          "Programming Language :: Python :: 3",
          "Programming Language :: Python :: 3.5",
          "Programming Language :: Python :: 3.6",
      ]
)
