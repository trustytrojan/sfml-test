#include "audioviz.hpp"
#include <numeric>

audioviz::audioviz(sf::Vector2u size, const std::string &audio_file, int antialiasing)
	: size(size),
	  audio_file(audio_file),
	  //   sf(audio_file),
	  //   ad(audio_file),
	  ps(size, 50),
	  title_text(font, ad.get_metadata_entry("title")),
	  artist_text(font, ad.get_metadata_entry("artist")),
	  rt(size, antialiasing)
{
	if (!font.loadFromFile("/usr/share/fonts/TTF/Iosevka-Regular.ttc"))
		throw std::runtime_error("failed to load font!");
	title_text.setStyle(sf::Text::Italic);
	artist_text.setStyle(sf::Text::Italic);

	title_text.setCharacterSize(32);
	artist_text.setCharacterSize(24);

	title_text.setFillColor({255, 255, 255, 150});
	artist_text.setFillColor({255, 255, 255, 150});

	if (!album_cover.texture.loadFromFile("images/obsessed.jpg"))
		throw std::runtime_error("failed to load album cover!");
	album_cover.sprite.setTexture(album_cover.texture, true);

	const sf::Vector2f ac_pos{30, 30};
	album_cover.sprite.setPosition(ac_pos);

	// using album_cover.texture.getSize(), set the scale on album_cover.sprite
	// such that it will only take up a 50x50 area
	const sf::Vector2f ac_size{150, 150};
	const auto ac_tsize = album_cover.texture.getSize();
	album_cover.sprite.setScale({ac_size.x / ac_tsize.x, ac_size.y / ac_tsize.y});

	const sf::Vector2f metadata_pos{ac_pos.x + ac_size.x + 10, ac_pos.y};
	title_text.setPosition({metadata_pos.x, metadata_pos.y});
	artist_text.setPosition({metadata_pos.x, metadata_pos.y + title_text.getCharacterSize() + 10});

	if (ad.nb_channels() != 2)
		throw std::runtime_error("only stereo audio is supported!");
	
	// TODO: multithread this
	ad.decode_entire_file(full_audio);
}

void audioviz::set_framerate(int framerate)
{
	this->framerate = framerate;
	// afpvf = sf.samplerate() / framerate;
	afpvf = ad.sample_rate() / framerate;
}

void audioviz::set_bg(const std::string &file)
{
	sf::Texture bg_texture;

	if (!bg_texture.loadFromFile(file))
		throw std::runtime_error("failed to load background image: '" + file + '\'');

	// sprites do not receive texture changes, so need to reset the texture rect
	// bg_sprite.setTexture(bg_texture, true);

	sf::Sprite bg_sprite(bg_texture);

	// make sure bgTexture fills up the whole screen, and is centered
	const auto tsize = bg_texture.getSize();
	bg_sprite.setOrigin({tsize.x / 2, tsize.y / 2});
	bg_sprite.setPosition({size.x / 2, size.y / 2});
	const auto scale = std::max((float)size.x / tsize.x, (float)size.y / tsize.y);
	bg_sprite.setScale({scale, scale});

	rt.bg.draw(bg_sprite);
	rt.bg.blur(10, 10, 10);
	rt.bg.multiply(0.5);
}

Pa::Stream<float> audioviz::create_pa_stream()
{
	// return Pa::Stream<float>(0, sf.channels(), sf.samplerate(), sample_size);
	return Pa::Stream<float>(0, ad.nb_channels(), ad.sample_rate(), sample_size);
}

static void debug_rects(sf::RenderTarget &target, const sf::IntRect &left_half, const sf::IntRect &right_half)
{
	sf::RectangleShape r1((sf::Vector2f)left_half.getSize()), r2((sf::Vector2f)right_half.getSize());
	r1.setPosition((sf::Vector2f)left_half.getPosition());
	r2.setPosition((sf::Vector2f)right_half.getPosition());
	r1.setFillColor(sf::Color{0});
	r2.setFillColor(sf::Color{0});
	r1.setOutlineThickness(1);
	r2.setOutlineThickness(1);
	target.draw(r1);
	target.draw(r2);
}

bool audioviz::draw_frame(sf::RenderTarget &target, Pa::Stream<float> *const pa_stream)
{
	static const sf::Color zero_alpha(0);

	// TODO: get rid of this, maybe add a rect parameter????
	// that would make this even more modular!!!!!!!!!!!
	static const auto margin = 10;

	if (target.getSize() != size)
		throw std::runtime_error("target size must match render-texture size!");

	// read audio from file
	// const auto frames_read = sf.readf(audio_buffer.data(), sample_size);
	// const auto frames_read = ad.read_n_frames(audio_buffer, sample_size);

	// no audio left, we are done
	// if (!frames_read)
	// if (!ad.read_n_frames(audio_buffer, sample_size))
	if (full_audio_idx + afpvf >= full_audio.size())
		return false;

	try // to play the audio
	{
		if (pa_stream)
			pa_stream->write(full_audio.data() + full_audio_idx, afpvf);
		// pa_stream->write(audio_buffer, afpvf);
		// *pa_stream << audio_buffer;
	}
	catch (const Pa::Error &e)
	{
		if (e.code != paOutputUnderflowed)
			throw;
		std::cerr << "output underflowed\n";
	}

	// not enough audio to perform fft, we are done
	// if (frames_read != sample_size)
	// if ((int)audio_buffer.size() != sample_size)
	if (full_audio.size() - full_audio_idx < sample_size)
		return false;

	// stereo rectangles to draw to
	const sf::IntRect
		left_half{
			{margin, margin},
			{(size.x - (2 * margin)) / 2.f - (sd.bar.get_spacing() / 2.f), size.y - (2 * margin)}},
		right_half{
			{(size.x / 2.f) + (sd.bar.get_spacing() / 2.f), margin},
			{(size.x - (2 * margin)) / 2.f - (sd.bar.get_spacing() / 2.f), size.y - (2 * margin)}};

	// draw spectrum on rt.original
	rt.spectrum.original.clear(zero_alpha);

	const auto dist_between_rects = right_half.left - (left_half.left + left_half.width);
	assert(dist_between_rects == sd.bar.get_spacing());

	// sd.copy_channel_to_input(audio_buffer.data(), 2, 0, true);
	sd.copy_channel_to_input(full_audio.data() + full_audio_idx, 2, 0, true);
	sd.draw(rt.spectrum.original, left_half, true);

	const auto &spectrum = sd.get_spectrum();
	const auto amount_to_avg = spectrum.size() / 4.f;
	float speeds[2];
	speeds[0] = std::accumulate(spectrum.begin(), spectrum.begin() + amount_to_avg, 0.f) / amount_to_avg;

	// sd.copy_channel_to_input(audio_buffer.data(), 2, 1, true);
	sd.copy_channel_to_input(full_audio.data() + full_audio_idx, 2, 1, true);
	sd.draw(rt.spectrum.original, right_half);
	speeds[1] = std::accumulate(spectrum.begin(), spectrum.begin() + amount_to_avg, 0.f) / amount_to_avg;

	const auto speeds_avg = (speeds[0] + speeds[1]) / 2;

	rt.particles.original.clear(zero_alpha);
	// const auto speed_increase = sqrtf(sqrtf(size.y * speeds_avg));
	const auto speed_increase =
		// cbrtf(size.y * speeds_avg);
		powf(size.y * speeds_avg, 1.f / 2.6666667f);
	ps.draw(rt.particles.original, {0, -speed_increase});
	rt.particles.original.display();

	rt.spectrum.original.display();

	// perform blurring where needed
	rt.spectrum.blurred.clear(zero_alpha);
	rt.spectrum.blurred.draw(rt.spectrum.original.sprite);
	rt.spectrum.blurred.display();
	rt.spectrum.blurred.blur(1, 1, 15);

	rt.particles.blurred.clear(zero_alpha);
	rt.particles.blurred.draw(rt.particles.original.sprite);
	rt.particles.blurred.display();
	rt.particles.blurred.blur(1, 1, 10);

	// layer everything together
	// target.draw(bg.sprite);
	target.draw(rt.bg.sprite);
	target.draw(rt.particles.blurred.sprite, sf::BlendAdd);
	target.draw(rt.particles.original.sprite, sf::BlendAdd);

	// new discovery...........................
	// this is how to invert the color??????
	sf::BlendMode spectrum_blend(
		sf::BlendMode::Factor::SrcAlpha,
		sf::BlendMode::Factor::DstAlpha,
		sf::BlendMode::Equation::ReverseSubtract);
	
	target.draw(rt.spectrum.blurred.sprite, spectrum_blend);

	// redraw original spectrum over everything else
	// need to do this because anti-aliased edges will copy the
	// background they are drawn on, completely ignoring alpha values.
	// i really need to separate the actions SpectrumDrawable::draw takes.
	
	sd.copy_channel_to_input(full_audio.data() + full_audio_idx, 2, 0, true);
	sd.draw(target, left_half, true);
	sd.copy_channel_to_input(full_audio.data() + full_audio_idx, 2, 1, true);
	sd.draw(target, right_half);

	target.draw(album_cover.sprite);
	target.draw(title_text, sf::BlendAlpha);
	target.draw(artist_text, sf::BlendAlpha);

	// seek audio backwards
	// sf.seek(afpvf - sample_size, SEEK_CUR);
	// ad.seek(afpvf - sample_size, SEEK_CUR);

	// NEED TO MULTIPLY BY 2 BECAUSE 2 IS THE NUMBER OF CHANNELS
	// AND FULL_AUDIO IS INTERLEAVED AUDIO
	full_audio_idx += 2 * afpvf;
	return true;
}

void audioviz::set_bar_width(int width)
{
	sd.bar.set_width(width);
}

void audioviz::set_bar_spacing(int spacing)
{
	sd.bar.set_spacing(spacing);
}

void audioviz::set_color_mode(SD::ColorMode mode)
{
	sd.color.set_mode(mode);
}

void audioviz::set_solid_color(sf::Color color)
{
	sd.color.set_solid_rgb(color);
}

void audioviz::set_color_wheel_rate(float rate)
{
	sd.color.wheel.set_rate(rate);
}

void audioviz::set_color_wheel_hsv(sf::Vector3f hsv)
{
	sd.color.wheel.set_hsv(hsv);
}

void audioviz::set_multiplier(float multiplier)
{
	sd.set_multiplier(multiplier);
}

void audioviz::set_fft_size(int fft_size)
{
	sd.set_fft_size(fft_size);
}

void audioviz::set_interp_type(FS::InterpolationType interp_type)
{
	sd.set_interp_type(interp_type);
}

void audioviz::set_scale(FS::Scale scale)
{
	sd.set_scale(scale);
}

void audioviz::set_nth_root(int nth_root)
{
	sd.set_nth_root(nth_root);
}

void audioviz::set_accum_method(FS::AccumulationMethod method)
{
	sd.set_accum_method(method);
}

void audioviz::set_window_func(FS::WindowFunction wf)
{
	sd.set_window_func(wf);
}