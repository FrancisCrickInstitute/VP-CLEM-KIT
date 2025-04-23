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
    axs[0, 0].set_xlabel('Photons')
    sns.histplot(ax=axs[0, 1], data=dataset_original.sx, binrange=(0, max_sigma_hist), color='blue')
    axs[0, 1].set_title('Localization uncertainty (sigma)')
    axs[0, 1].set_xlabel('s0 (pixels)')
    sns.histplot(ax=axs[1, 0], data=dataset_original.bg, binrange=(0, max_bkg_hist), color='black')
    axs[1, 0].set_title('Background of localizations')
    axs[1, 0].set_xlabel('Background (photons)')
    sns.histplot(ax=axs[1, 1], data=dataset_original.frame, binrange=(0, max_frame_hist), color='yellow')
    axs[1, 1].set_title('Localizations per frame')
    axs[1, 1].set_xlabel('Frame')

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


def plot_histograms_filter(dataset_original, gui_filter_render):

    max_photons_hist, max_sigma_hist, max_bkg_hist, max_frame_hist = processing.define_max_hist(dataset_original)

    lower_bound_sigma, upper_bound_sigma, lower_bound_photons, upper_bound_photons, lower_bound_bkg, upper_bound_bkg, min_frame, max_frame, _ ,_ ,_ = processing.read_slidebars_filter(gui_filter_render)

    fig, axsh = plt.subplots(2, 2, figsize=(15, 5))

    sns.histplot(ax=axsh[0, 0], data=dataset_original.photons, binrange=(0, max_photons_hist), color='red')
    # set x axis 
    axsh[0, 0].set_xlim(lower_bound_photons*0.1 , upper_bound_photons + upper_bound_photons*0.1)
    # add vertical lines for the lower and upper bounds
    axsh[0, 0].axvline(x=lower_bound_photons, color='black', linestyle='--')
    axsh[0, 0].axvline(x=upper_bound_photons, color='black', linestyle='--')
    axsh[0, 0].set_title('Filtered photons')
    axsh[0, 0].set_xlabel('Photons')

    sns.histplot(ax=axsh[0, 1], data=dataset_original.sx, binrange=(0, max_sigma_hist), color='blue')
    axsh[0, 1].set_xlim(lower_bound_sigma*0.1 , upper_bound_sigma + upper_bound_sigma*0.1)
    # add vertical lines for the lower and upper bounds
    axsh[0, 1].axvline(x=lower_bound_sigma, color='black', linestyle='--')
    axsh[0, 1].axvline(x=upper_bound_sigma, color='black', linestyle='--')
    axsh[0, 1].set_title('Filtered localization uncertainty (sigma)')
    axsh[0, 1].set_xlabel('s0 (pixels)')

    sns.histplot(ax=axsh[1, 0], data=dataset_original.bg, binrange=(0, max_bkg_hist), color='black')
    axsh[1, 0].set_xlim(lower_bound_bkg*0.1 , upper_bound_bkg + upper_bound_bkg*0.1)
    # add vertical lines for the lower and upper bounds
    axsh[1, 0].axvline(x=lower_bound_bkg, color='black', linestyle='--')
    axsh[1, 0].axvline(x=upper_bound_bkg, color='black', linestyle='--')
    axsh[1, 0].set_title('Background of localizations')
    axsh[1, 0].set_xlabel('Background (photons)')

    sns.histplot(ax=axsh[1,1], data=dataset_original.frame, binrange=(0, max_frame_hist), color='yellow')
    # add vertical lines for the lower and upper bounds
    axsh[1, 1].axvline(x=min_frame, color='black', linestyle='--')
    axsh[1, 1].axvline(x=max_frame, color='black', linestyle='--')
    axsh[1, 1].set_title('Localizations per frame')
    axsh[1, 1].set_xlabel('Frame')
    # put a text box in upper right in axes coords with the total number of localizations
    axsh[1, 1].text(0.95, 0.95, f"Total number of localizations: {dataset_original.shape[0]}", transform=axsh[1, 1].transAxes, ha='right', va='top')

    plt.tight_layout()
    
    img_buf = io.BytesIO()
    plt.savefig(img_buf, format="jpeg")
    output_plot = widgets.Output()
    with output_plot:
        display(Image.open(img_buf))

    return output_plot

def write_params_to_file(gui_filter_render, path, file_save):
    
    lower_bound_sigma, upper_bound_sigma, lower_bound_photons, upper_bound_photons, lower_bound_bkg, upper_bound_bkg, min_frame, max_frame, oversampling, roi_size, use_full_roi = processing.read_slidebars_filter(gui_filter_render)
    radius_filter = float(gui_filter_render["density_radius"].value)
    num_neighbours = int(gui_filter_render["density_num_neighbours"].value)
    
    # Write a text file with the parameters

    with open(os.path.join(path, file_save + "_parameters_filter.txt"), "w") as f:
        f.write(f"GENERAL FILTER PARAMETERS:\n")
        f.write(f"Lower bound sigma: {lower_bound_sigma}\n")
        f.write(f"Upper bound sigma: {upper_bound_sigma}\n")
        f.write(f"Lower bound photons: {lower_bound_photons}\n")
        f.write(f"Upper bound photons: {upper_bound_photons}\n")
        f.write(f"Lower bound background: {lower_bound_bkg}\n")
        f.write(f"Upper bound background: {upper_bound_bkg}\n")
        f.write(f"Lower bound frame: {min_frame}\n")
        f.write(f"Upper bound frame: {max_frame}\n")
        f.write(f"--------------------\n")
        f.write(f"DENSITY FILTER PARAMETERS:\n")
        f.write(f"Radius filter: {radius_filter}\n")
        f.write(f"Number of neighbours: {num_neighbours}\n")
        f.write(f"--------------------\n")
        f.write(f"RENDERING PARAMETERS:\n")
        f.write(f"Oversampling: {oversampling}\n")
        f.write(f"ROI size: {roi_size}\n")
        f.write(f"Use full ROI: {use_full_roi}\n")
