import io
import os

from matplotlib import colormaps
from matplotlib import pyplot as plt
import seaborn as sns
from pylab import *

from IPython.display import display
import ipywidgets as widgets
from PIL import Image

from . import processing

def plot_histograms(dataset_original):

    max_photons_hist, max_sigma_hist, max_bkg_hist, max_frame_hist = processing.define_max_hist(dataset_original)

    fig, axs = plt.subplots(2, 2, figsize=(15, 5))
    sns.histplot(ax=axs[0, 0], data=dataset_original.photons, binrange=(0, max_photons_hist), color='red')
    axs[0, 0].set_title('Photons')
    sns.histplot(ax=axs[0, 1], data=dataset_original.sx, binrange=(0, max_sigma_hist), color='blue')
    axs[0, 1].set_title('Localization uncertainty (sigma)')
    sns.histplot(ax=axs[1, 0], data=dataset_original.bg, binrange=(0, max_bkg_hist), color='black')
    axs[1, 0].set_title('Background of localizations')
    sns.histplot(ax=axs[1, 1], data=dataset_original.frame, binrange=(0, max_frame_hist), color='yellow')
    axs[1, 1].set_title('Localizations per frame')

    plt.tight_layout()

    img_buf = io.BytesIO()
    plt.savefig(img_buf, format="jpeg")

    output_plot = widgets.Output()
    with output_plot:
        display(Image.open(img_buf))
                
    plt.clf()

    return output_plot

def plot_and_save_histogram(x, file_save, title, color, xlim, path):
    fig, ax = plt.subplots()
    transparent = False

    ax.set_xlim(xlim)
    y, x, _ = hist(x, 1000, alpha=0.3, label='data', color=color) 
    plt.title(title)
    fig.savefig(os.path.join(path, f"{file_save}_{title.lower()}.png"), transparent=transparent)


def plot_histograms_filter(dataset_original, photons_low, photons_high, sigma_low, sigma_high, bkg_low, bkg_high, frame_low, frame_high):

    max_photons_hist, max_sigma_hist, max_bkg_hist, max_frame_hist = processing.define_max_hist(dataset_original)

    fig, axsh = plt.subplots(2, 2, figsize=(15, 5))

    sns.histplot(ax=axsh[0, 0], data=dataset_original.photons, binrange=(0, max_photons_hist), color='red')
    # set x axis 
    axsh[0, 0].set_xlim(photons_low*0.1 , photons_high + photons_high*0.1)
    # add vertical lines for the lower and upper bounds
    axsh[0, 0].axvline(x=photons_low, color='black', linestyle='--')
    axsh[0, 0].axvline(x=photons_high, color='black', linestyle='--')
    axsh[0, 0].set_title('Filtered photons')

    sns.histplot(ax=axsh[0, 1], data=dataset_original.sx, binrange=(0, max_sigma_hist), color='blue')
    axsh[0, 1].set_xlim(sigma_low*0.1 , sigma_high + sigma_high*0.1)
    # add vertical lines for the lower and upper bounds
    axsh[0, 1].axvline(x=sigma_low, color='black', linestyle='--')
    axsh[0, 1].axvline(x=sigma_high, color='black', linestyle='--')
    axsh[0, 1].set_title('Filtered localization uncertainty (sigma)')

    sns.histplot(ax=axsh[1, 0], data=dataset_original.bg, binrange=(0, max_bkg_hist), color='black')
    axsh[1, 0].set_xlim(bkg_low*0.1 , bkg_high + bkg_high*0.1)
    # add vertical lines for the lower and upper bounds
    axsh[1, 0].axvline(x=bkg_low, color='black', linestyle='--')
    axsh[1, 0].axvline(x=bkg_high, color='black', linestyle='--')
    axsh[1, 0].set_title('Background of localizations')

    sns.histplot(ax=axsh[1,1], data=dataset_original.frame, binrange=(0, max_frame_hist), color='yellow')
    # add vertical lines for the lower and upper bounds
    axsh[1, 1].axvline(x=frame_low, color='black', linestyle='--')
    axsh[1, 1].axvline(x=frame_high, color='black', linestyle='--')
    axsh[1, 1].set_title('Localizations per frame')

    plt.tight_layout()
    
    img_buf = io.BytesIO()
    plt.savefig(img_buf, format="jpeg")
    output_plot = widgets.Output()
    with output_plot:
        display(Image.open(img_buf))

    return output_plot