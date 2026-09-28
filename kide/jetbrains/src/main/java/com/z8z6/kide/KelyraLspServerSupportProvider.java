package com.z8z6.kide;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.platform.lsp.api.LspServerSupportProvider;
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor;
import org.jetbrains.annotations.NotNull;

public final class KelyraLspServerSupportProvider
    implements LspServerSupportProvider {
  @Override
  public void fileOpened(
      @NotNull Project project,
      @NotNull VirtualFile file,
      @NotNull LspServerStarter serverStarter) {
    if ("kly".equals(file.getExtension())) {
      serverStarter.ensureServerStarted(new KelyraLspServerDescriptor(project));
    }
  }

  private static final class KelyraLspServerDescriptor
      extends ProjectWideLspServerDescriptor {
    private KelyraLspServerDescriptor(Project project) {
      super(project, "Kelyra");
    }

    @Override
    public boolean isSupportedFile(@NotNull VirtualFile file) {
      return "kly".equals(file.getExtension());
    }

    @Override
    public @NotNull GeneralCommandLine createCommandLine() {
      return new GeneralCommandLine("kelyra-ls", "--stdio");
    }
  }
}
