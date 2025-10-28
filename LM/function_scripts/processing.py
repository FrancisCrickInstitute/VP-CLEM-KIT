import io
import os

import numpy as np
from matplotlib import pyplot as plt
import seaborn as sns
import tifffile
from pylab import *
from scipy.spatial import cKDTree
import time

from IPython.display import display
import ipywidgets as widgets
from PIL import Image

from picasso import render

from . import plotting

def read_slidebars_filter(gui_filter_render):

    lower_bound_sigma = float(gui_filter_render["sigma_low"].value)
    upper_bound_sigma = float(gui_filter_render["sigma_high"].value)
    lower_bound_photons = float(gui_filter_render["photons_low"].value)
    upper_bound_photons = float(gui_filter_render["photons_high"].value)
    lower_bound_bkg = float(gui_filter_render["bkg_low"].value)
    upper_bound_bkg = float(gui_filter_render["bkg_high"].value)
    min_frame = float(gui_filter_render["frames_low"].value)
    max_frame = float(gui_filter_render["frames_high"].value)

    oversampling = int(gui_filter_render["magnification"].value)
    roi_size = int(gui_filter_render["roi_size"].value)
    use_full_roi = gui_filter_render["use_full_roi"].value

    return lower_bound_sigma, upper_bound_sigma, lower_bound_photons, upper_bound_photons, lower_bound_bkg, upper_bound_bkg, min_frame, max_frame, oversampling, roi_size, use_full_roi

def compute_uncertainty(dataset, pixel_size_um):

    # Apply Thomson formula to compute uncertainty from https://doi.org/10.1016/S0006-3495(02)75618-X
    # Only for experimental purposes at the moment

    pixel_size = pixel_size_um * 1e-6 # in m

    avg_s0_px = (dataset.sx + dataset.sy)/2 # in pixels
    avg_s0 = avg_s0_px*pixel_size # in m

    avg_sigmas2 = avg_s0*avg_s0 + (pixel_size**2/12)

    uncertainty2 = avg_sigmas2 / dataset['photons'] * (16/9 + 8*np.pi*avg_sigmas2*dataset['bg']**2/dataset['photons']/pixel_size/pixel_size)

    uncertainty = np.sqrt(uncertainty2) * 1e6 # in um

    dataset['sx'] = uncertainty
    dataset['sy'] = dataset['sx']

    return dataset

def find_optimal_bkg(dataset_original):

    mean_bkg = np.mean(dataset_original.bg)
    std_bkg = np.std(dataset_original.bg)

    print("Mean bkg:", mean_bkg)
    print("Std bkg:", std_bkg)

    return mean_bkg-std_bkg, mean_bkg+2*std_bkg

def find_optimal_photons(dataset_original):

    mean_photons = np.median(dataset_original.photons)
    std_photons = np.std(dataset_original.photons)

    return mean_photons-std_photons

def define_max_hist(dataset_original):

    global max_photons_hist
    global max_sigma_hist
    global max_bkg_hist
    global max_frame_hist

    if np.max(dataset_original.photons) > np.mean(dataset_original.photons)*20:
        print("Max number of photons too high, setting it automatically to 20000 for plotting")
        max_photons_hist = 20000
    else:
        max_photons_hist = np.max(dataset_original.photons)

    if np.max(dataset_original.sx) > np.mean(dataset_original.sx)*20:
        print("Max sx too high, setting it automatically to 10 for plotting")
        max_sigma_hist = 10
    else:
        max_sigma_hist = np.max(dataset_original.sx)

    max_bkg_hist = np.max(dataset_original.bg)
    max_frame_hist = np.max(dataset_original.frame)

    return max_photons_hist, max_sigma_hist, max_bkg_hist, max_frame_hist

def estimate_sigma(wavelength, NA_obj, pixel_size):

    FWHM_blobs = wavelength / 2 / NA_obj
    sigma_expected = FWHM_blobs / 2.355 / pixel_size # in pixel units 

    return sigma_expected

def render_data(locs_render, gui_data):

    # Render the image
    x_min = np.min(locs_render.x)
    x_max = np.max(locs_render.x)
    y_min = np.min(locs_render.y)
    y_max = np.max(locs_render.y)

    viewport_full = (y_min, x_min), (y_max, x_max)

    len_x, image = render.render(locs_render, viewport=viewport_full, oversampling=1, blur_method='gaussian_iso')

    fig, ax = plt.subplots()
    plt.imshow(image, interpolation = 'sinc', vmin = 0, vmax = max(image.flatten())*0.01, cmap=gui_data["cmaps"].value)
    plt.title('Rendered image before filtering')
    plt.axis("off")
    # plt.show()
    img_buf = io.BytesIO()
    plt.savefig(img_buf, format="jpeg")
    output_plot = widgets.Output()
    with output_plot:
        display(Image.open(img_buf))

    plt.clf()

    return output_plot

def filter_locs(locs, lower_bound_sigma, upper_bound_sigma, lower_bound_photons, upper_bound_photons, lower_bound_bkg, high_bound_bkg, min_frame, max_frame):

    if any(locs['sx'] != locs['sy']):
        # Filter all gaussians with either of the axes bigger than 3 times the other one
        print("Anisotropy filter elliminate locs with sx/sy or sy/sx > 2")
        print("Num locs before anisotropy filter ", len(locs))
        locs = locs[(locs['sx']/locs['sy'] < 2) | (locs['sy']/locs['sx'] < 2)]
        print("Num locs after anisotropy filter ", len(locs))

    print("Filtering out locs with uncertainty > ", lower_bound_sigma, "uncertainty < ", upper_bound_sigma, "and photons > ", lower_bound_photons, "and < ", upper_bound_photons, "and bkg < ", lower_bound_bkg, "and > ", high_bound_bkg)
    
    print("Num locs before general filter ", len(locs))
    locs = locs[(locs['frame'] > min_frame) & (locs['frame'] < max_frame)]
    locs = locs[(locs['sx'] > lower_bound_sigma) & (locs['sx'] < upper_bound_sigma) & (locs['sy'] > lower_bound_sigma) & (locs['sy'] < upper_bound_sigma)]
    locs = locs[(locs['photons'] > lower_bound_photons) & (locs['photons'] < upper_bound_photons)]
    locs = locs[(locs['bg'] > lower_bound_bkg) & (locs['bg'] < high_bound_bkg)]
    print("Num locs after general filter ", len(locs))

    return locs

def extract_histogram(path, file_save, locs, appendix):

    plotting.plot_and_save_histogram(locs.photons, file_save, 'Photon Distribution'+appendix, 'red', (0, max_photons_hist), path)
    plotting.plot_and_save_histogram(locs.sx, file_save, 'Uncertainty'+appendix, 'blue', (0, max_sigma_hist), path)

    # Plot detecttions per frame
    fig, ax = plt.subplots()
    sns.histplot(locs.frame, kde=False, color='orange')
    # Total number of localizations
    total_localizations = len(locs)
    plt.title('Detections per frame'+appendix+"with total_localizations = "+str(total_localizations))
    fig.savefig(os.path.join(path, f"{file_save}_detec_frame{appendix}.png"), transparent=False)
    plt.close('all')

def density_filter(dataset_original, radius, num_neighbours):

    """
    Filters localizations to include only those with at least `num_neighbours`
    within a radius.

    Parameters:
        localizations (numpy.ndarray): Array of shape (N, 2) containing (x, y) positions.
        radius (float): The radius within which to search for neighbors.
        num_neighbours (int): Minimum number of neighbors required within the radius.

    Returns:
        numpy.ndarray: Filtered array of localizations.
    """

    print("Starting density filter")
    t = time.time()

    localizations = np.column_stack((dataset_original['x'], dataset_original['y']))

    # Build a KDTree for efficient spatial queries
    tree = cKDTree(localizations)

    # Query the number of neighbors within the specified radius for each point
    counts = tree.query_ball_point(localizations, r=radius, return_length=True)

    # Keep only points with at least the required number of neighbors
    mask = counts >= num_neighbours

    elapsed = time.time() - t

    # Print the time it took, the localizatinos that were filtered and the number of localizations left

    print(f"Finished density filter in {elapsed:.2f} seconds. Filtered {len(localizations) - sum(mask)} localizations. {sum(mask)} localizations left.")

    return dataset_original[mask]


def extract_render_save(path, file_save, gui_filter_render, dataset_original, pixel_size):

    lower_bound_sigma, upper_bound_sigma, lower_bound_photons, upper_bound_photons, lower_bound_bkg, upper_bound_bkg, min_frame, max_frame, oversampling, roi_size, use_full_roi = read_slidebars_filter(gui_filter_render)

    extract_histogram(path, file_save, dataset_original, "_before filtering")

    locs_filtered = filter_locs(dataset_original, lower_bound_sigma, upper_bound_sigma, lower_bound_photons, upper_bound_photons, lower_bound_bkg, upper_bound_bkg, min_frame, max_frame)

    do_density_filter = gui_filter_render["Density_filter_option"].value

    if do_density_filter:
        radius_filter = float(gui_filter_render["density_radius"].value)
        num_neighbours = int(gui_filter_render["density_num_neighbours"].value)
        locs_filtered = density_filter(locs_filtered, radius_filter, num_neighbours)

    extract_histogram(path, file_save, locs_filtered, "_after filtering")

    # Render the image
    x_min = np.min(dataset_original.x)
    x_max = np.max(dataset_original.x)
    y_min = np.min(dataset_original.y)
    y_max = np.max(dataset_original.y)

    x_min_center = (x_max - x_min)//2 - roi_size//2
    x_max_center = (x_max - x_min)//2 + roi_size//2
    y_min_center = (y_max - y_min)//2 - roi_size//2
    y_max_center = (y_max - y_min)//2 + roi_size//2

    if use_full_roi:
        viewport = (y_min, x_min), (y_max, x_max)
    else:
        viewport = (y_min_center, x_min_center), (y_max_center, x_max_center)

    pixel_size_oversampling = pixel_size / oversampling

    # metadata = {
    # "PixelSizeX (um)": pixel_size/1000,  # in microns or any unit
    # "PixelSizeY (um)": pixel_size/1000,  # in microns or any unit

    # }

    # metadata_oversampling = {
    # "PixelSizeX (um)": pixel_size_oversampling/1000,  # in microns or any unit
    # "PixelSizeY (um)": pixel_size_oversampling/1000,  # in microns or any unit
    # }

    len_x, image = render.render(dataset_original, viewport=viewport, oversampling=1, blur_method='gaussian_iso')
    print("Saving redered image before filtering")
    tifffile.imwrite(os.path.join(path, f"{file_save}_rendered_before_filter.tif"), image, resolution=(25.4e6 / pixel_size , 25.4e6 / pixel_size))

    len_x, image_oversampling = render.render(locs_filtered, viewport=viewport, oversampling=oversampling, blur_method='gaussian_iso')
    print("Saving redered image after filtering")
    tifffile.imwrite(os.path.join(path, f"{file_save}_rendered.tif"), image_oversampling, resolution=(25.4e6 / pixel_size_oversampling, 25.4e6 / pixel_size_oversampling))

    return image_oversampling
