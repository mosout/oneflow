"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
from __future__ import absolute_import
from abc import ABC
from typing import Optional

from oneflow.python.framework.check_point_v2 import *
from oneflow.python.framework.function_util import api_oneflow_function
from oneflow.python.framework.module import Module
from oneflow.python.framework.session_util import api_clear_default_session
from oneflow.python.oneflow_export import oneflow_export
from oneflow.python.ops.optimizer import Optimizer


@oneflow_export("ModelCheckpointConfig")
class CheckpointConfig(object):
    def __init__(
        self,
        load_dirpath: str = None,
        save_dirpath: str = None,
        save_interval: int = 1,
    ):
        self.load_dirpath = load_dirpath
        self.save_dirpath = save_dirpath
        self.save_interval = save_interval


@oneflow_export("ModelCallback")
class Callback(ABC):
    r""" Abstract base class used to build new callbacks.
    """

    def on_training_step_end(self, step_idx, outputs, optimizer_idx):
        pass

    def on_validation_step_end(self, step_idx, outputs):
        pass


@oneflow_export("nn.NumpyModule")
class NumpyDataModule(Module):
    def forward(self, step_idx):
        pass


@oneflow_export("Model")
class Model(
    ABC, Module,
):
    r"""A high level API for model training and validation.
    """

    def __init__(self, *args, **kwargs):
        super().__init__()

        self.is_function_style = (
            kwargs["is_function_style"] if "is_function_style" in kwargs else False
        )
        if not self.is_function_style:
            raise NotImplementedError

        self.training_config = (
            kwargs["training_config"] if "training_config" in kwargs else None
        )
        self.validation_config = (
            kwargs["validation_config"] if "validation_config" in kwargs else None
        )
        self.callbacks = kwargs["callbacks"] if "callbacks" in kwargs else []

        self.need_training = False
        self.need_validation = False
        self.need_checkpoint = False

    def forward(self, *args, **kwargs):
        r"""Same as `nn.Module.forward()`, here is to define the operations you want to use for prediction.
        """
        return super().forward(*args, **kwargs)

    def training_step(self, *args, **kwargs):
        r"""Operates on a single batch of data from the training set and return loss.
        """
        raise NotImplementedError()

    def validation_step(self, *args, **kwargs):
        r"""Operates on a single batch of data from the validation set.
        """

    def configure_optimizers(self):
        r"""Choose what optimizers and learning-rate schedulers to use in your optimization.
        Normally you'd need one. But in the case of GANs or similar you might have multiple.
        """
        raise NotImplementedError()

    def _func_train_job(self, optimizer_idx=0):
        deco = api_oneflow_function(type="train", function_config=self.training_config)

        def job():
            batch = self.training_data()
            loss = self.training_step(batch=batch, optimizer_idx=optimizer_idx)
            self.optimizers[optimizer_idx].minimize(loss)
            return loss

        job.__name__ = (
            self.__class__.__name__ + "_Model_train_job_" + str(optimizer_idx)
        )

        return deco(job)

    def _func_eval_job(self):
        deco = api_oneflow_function(function_config=self.validation_config)

        def job():
            batch = self.validation_data()
            return self.validation_step(batch)

        job.__name__ = self.__class__.__name__ + "_Model_eval_job"

        return deco(job)

    def _method_overrided(self, method_name: str = None) -> bool:
        return getattr(self.__class__, method_name) != getattr(Model, method_name)

    def _method_callback(self, method_name: str = None, *args, **kwargs):
        for cb in self.callbacks:
            method = getattr(cb, method_name)
            method(*args, **kwargs)

    def fit(
        self,
        training_data=None,
        validation_data=None,
        validation_interval: int = 1,
        checkpoint_config=None,
        max_steps: int = 100,
    ):
        api_clear_default_session()

        self.max_steps = max_steps
        self.training_data = training_data
        self.validation_data = validation_data
        self.validation_interval = validation_interval
        self.checkpoint_config = checkpoint_config

        self.is_numpy_input = (
            True if isinstance(self.training_data, NumpyDataModule) else False
        )

        optim_conf = self.configure_optimizers()
        if isinstance(optim_conf, Optimizer):
            self.optimizers = [optim_conf]
        elif isinstance(optim_conf, (list, tuple)):
            self.optimizers = optim_conf

        self.train_jobs = []

        # construct training job
        if self._method_overrided("training_step") and self.training_data is not None:
            self.need_training = True
            if not self.is_numpy_input:
                if len(self.optimizers) == 1:
                    self.train_jobs.append(self._func_train_job(0))
                else:
                    for optimizer_idx in range(0, len(self.optimizers)):
                        self.train_jobs.append(self._func_train_job(optimizer_idx))
            else:
                raise NotImplementedError

        # construct validation job
        if (
            self._method_overrided("validation_step")
            and self.validation_data is not None
        ):
            if not self.is_numpy_input:
                self.need_validation = True
                self.eval_job = self._func_eval_job()
            else:
                raise NotImplementedError

        # return True

        if self.checkpoint_config.load_dirpath is not None:
            self._load_checkpoint(dirpath=self.checkpoint_config.load_dirpath)

        if self.checkpoint_config.save_dirpath is not None:
            self.need_checkpoint = True

        for step_idx in range(0, self.max_steps):
            if self.need_training:
                for optimizer_idx in range(0, len(self.optimizers)):
                    loss = self.train_jobs[optimizer_idx]().get()
                    self._method_callback(
                        "on_training_step_end",
                        step_idx=step_idx,
                        outputs=loss,
                        optimizer_idx=optimizer_idx,
                    )

            if self.need_validation:
                if (step_idx + 1) % self.validation_interval == 0:
                    eval_loss = self.eval_job().get()
                    self._method_callback(
                        "on_validation_step_end", step_idx=step_idx, outputs=eval_loss
                    )

            if self.need_checkpoint:
                if (step_idx + 1) % self.checkpoint_config.save_interval == 0:
                    self._save_checkpoint(
                        dirpath=self.checkpoint_config.save_dirpath
                        + "-"
                        + str(step_idx)
                    )

    def _save_checkpoint(
        self, dirpath,
    ):
        r"""Save model states as a checkpoint.
        """
        SaveVarDict(path=dirpath)

    def _load_checkpoint(
        self, dirpath,
    ):
        r"""Load model states from a checkpoint.
        """
        LoadVariables(GetCheckpoint(path=dirpath))
