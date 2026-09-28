package com.z8z6.kide;

import com.intellij.ide.plugins.IdeaPluginDescriptor;
import com.intellij.ide.plugins.PluginManagerCore;
import com.intellij.openapi.extensions.PluginId;
import java.util.List;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.plugins.textmate.api.TextMateBundleProvider;

public final class KelyraBundleProvider implements TextMateBundleProvider {
  @Override
  public @NotNull List<PluginBundle> getBundles() {
    IdeaPluginDescriptor plugin =
        PluginManagerCore.getPlugin(PluginId.getId("com.z8z6.kide"));
    if (plugin == null) {
      return List.of();
    }
    return List.of(
        new PluginBundle("Kelyra", plugin.getPluginPath().resolve("textmate")));
  }
}
