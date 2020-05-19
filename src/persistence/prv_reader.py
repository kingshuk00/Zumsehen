import logging
import os
import subprocess
from datetime import datetime

import h5py

from src.persistence.prv_to_hdf5 import ParaverToHDF5
from src.persistence.reader import Reader
from src.persistence.writer import Writer
from src.TraceMetaData import TraceMetaData

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

PARAVER_FILE = "Paraver (.prv)"
PARAVER_MAGIC_HEADER = "#Paraver"


class ParaverReader(Reader):
    def header_date(self, header: str):
        date, _, other = header.replace("#Paraver (", "").replace("at ", "").partition("):")
        date = datetime.strptime(date, "%d/%m/%Y %H:%M")
        return date

    def header_time(self, header):
        time, _, other = header[header.find("):") + 2 :].partition("_ns")
        time = int(time)
        return time

    def header_nodes(self, header: str):
        nodes = header[header.find("_ns:") + 4 :]
        if nodes[0] == "0":
            nodes = None
        else:
            nodes = nodes[nodes.find("(") + 1 : nodes.find(")")]
            nodes = nodes.split(",")
            nodes = list(map(int, nodes))
        return nodes

    def header_apps(self, header: str):
        apps_list = []
        apps = header[header.find("_ns:") + 4 :]
        apps, _, other = apps.partition(":")
        apps, _, other = other.partition(":")
        number_apps = int(apps)
        i = 0
        while i < number_apps:
            apps, _, other = other.partition("(")
            number_tasks = int(apps)
            apps, _, other = other.partition(")")
            apps = apps.split(",")
            j = 0
            tasks_list = []
            while j < number_tasks:
                tmp = list(map(int, apps[j].split(":")))
                tasks_list.append(dict(nThreads=tmp[0], node=tmp[1]))
                j += 1
            apps_list.append(tasks_list)
            i += 1
            apps, _, other = other.partition(":")
        return apps_list

    def header_parser(self, header: str):
        header_time = self.header_time(header)
        header_date = self.header_date(header)
        header_nodes = self.header_nodes(header)
        header_apps = self.header_apps(header)
        return header_time, header_date, header_nodes, header_apps

    def parse_content(self, file: str, output_hdf5: str):
        try:
            subprocess.run(["./src/persistence/prv_reader", file, output_hdf5], check=True)
        except subprocess.CalledProcessError:
            logger.warning("C paraver reader failed, fallbacking to Python.")
            df_state, df_event, df_comm = ParaverToHDF5().parse_as_dataframe(file, use_dask=True)
            Writer().dataframe_to_hdf5(output_hdf5, df_state, df_event, df_comm)

    def write_metadata_to_hdf5(self, file_hdf5, trace_metadata: TraceMetaData):
        with h5py.File(file_hdf5, "a") as f:
            records = f["RECORDS"]
            records.attrs["name"] = trace_metadata.name
            records.attrs["path"] = trace_metadata.path
            records.attrs["type"] = trace_metadata.type
            records.attrs["exec_time"] = trace_metadata.exec_time
            records.attrs["date_time"] = trace_metadata.date_time.isoformat()
            records.attrs["nodes"] = trace_metadata.nodes
            apps_str = []
            for app in apps_str:
                apps_str.append([])
                for task in app:
                    apps_str[-1].append(str(task))
            records.attrs["apps"] = apps_str

    def parse_file(self, file: str) -> TraceMetaData:
        try:
            with open(file, "r") as f:
                header = f.readline()
                if PARAVER_MAGIC_HEADER not in header:
                    logger.error(f"The file {f.name} is not a valid Paraver file!")

                logger.info(f"Parsing {f}")
                trace_name = os.path.basename(f.name)
                trace_path = os.path.abspath(f.name)
                trace_type = PARAVER_FILE

                trace_exec_time, trace_date, trace_nodes, trace_apps = self.header_parser(header)
                output_hdf5 = file.replace(".prv", ".hdf")
                self.parse_content(file, output_hdf5)
                trace_metadata = TraceMetaData(
                    trace_name, trace_path, trace_type, trace_exec_time, trace_date, trace_nodes, trace_apps
                )
                self.write_metadata_to_hdf5(output_hdf5, trace_metadata)
        except FileNotFoundError:
            logger.error(f"Not able to access the file {file}")
            raise

        return trace_metadata


if __name__ == "__main__":
    ParaverReader().parse_file("src/persistence/test/test_files/traces/10MB.test.prv")
