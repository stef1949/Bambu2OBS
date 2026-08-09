# Third-party notices

This inventory covers direct runtime dependencies pinned in `requirements.txt` and the planned native OBS linkage. It is not a substitute for the licence texts shipped by each dependency, and transitive dependencies must also be inventoried before distributing an installer or portable archive.

| Component | Version / target | Declared licence | Project |
| --- | --- | --- | --- |
| pybambu | 1.0.1 | Apache Software License classifier | <https://github.com/greghesp/pybambu> |
| bambu-lab-cloud-api | 1.0.5 | AGPL-3.0 | <https://github.com/coelacant1/Bambu-Lab-Cloud-API> |
| Requests | 2.32.5 | Apache-2.0 | <https://requests.readthedocs.io> |
| Eclipse Paho MQTT | 2.1.0 | EPL-2.0 OR BSD-3-Clause | <https://eclipse.dev/paho/> |
| python-dotenv | 1.2.1 | BSD-3-Clause | <https://github.com/theskumar/python-dotenv> |
| Flask | 3.1.2 | BSD-3-Clause | <https://github.com/pallets/flask> |
| OBS Studio / libobs | Supported native build target | GPL-2.0-or-later project terms | <https://github.com/obsproject/obs-studio> |

## Release requirement

Before publishing either runtime:

1. collect the exact licence and notice files for every packaged direct and transitive dependency;
2. review the AGPL cloud-client dependency and libobs linkage against the repository's current MIT licence and intended distribution model;
3. record the chosen compatible licensing approach rather than silently changing the existing licence;
4. include all required source offers, notices, and licence texts in the release artifact.
